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
//
// SEVERAL BOOKS, AND SHARED ONES (schema 6079, AddressBookStore.h). Every
// contact is in an address book, and book= says which one this request is
// about:
//
//   absent   every book the account OWNS, which with no books made is exactly
//            what this route answered before 6079 and exactly what the CSV and
//            vCard exports still mean by "my contacts".
//   <id>     that one book: one of the account's own, or one shared with it.
//   all      every book it can see, its own first and then the shared ones,
//            each contact carrying the book it is in - which is what the To
//            field's completion asks for, so that a colleague in a team's book
//            is offered and is labelled with whose book it came from.
//
// A contact of a book somebody shares belongs to the book's OWNER: it is read
// and written under the owner's account id, and a book shared read-only refuses
// every write with 403.

#include "StdAfx.h"

#include <map>
#include <memory>

#include "RestApiServer.h"
#include "HttpServer.h"
#include "AddressBookStore.h"
#include "ContactStore.h"
#include "ContactCard.h"
#include "ContactGroups.h"
#include "InertMediaType.h"
#include "Unicode.h"
#include "VCard.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"

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

      // The three fields a listing puts on every contact when more than one
      // book is in the answer, so the page can say where each came from.
      AnsiString BookFields(const RestApiServer::VisibleBook &book)
      {
         return AnsiString(",\"book_id\":") + Int64Text(book.id) +
            ",\"book_name\":\"" + RestApiServer::JsonEscapeUtf8(book.name) +
            "\",\"book_owner\":\"" + RestApiServer::JsonEscapeUtf8(book.owner) +
            "\",\"book_mine\":" + (book.mine ? "true" : "false") +
            ",\"book_writable\":" + (book.writable ? "true" : "false");
      }
   }

   // A contact as the page reads it: the row, and what the card a phone
   // stored carries beside the name and address - the birthday, the note, the
   // phones, whether there is a photo (served at /photo, since it is an image
   // and not something to hand out a thousand times in a listing) - and the
   // card itself when asked for (fields=card), so an export carries it whole.
   // book, when there is one, says which address book the contact is in.
   AnsiString
   RestApiServer::ContactJson_(const ContactRecord &contact, bool includeCard, const VisibleBook *book)
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

      if (book)
         json += BookFields(*book);

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
      // stored card on each contact; book= is one book's id, "all" for every
      // book the account can see, or absent for its own books.
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

      std::vector<VisibleBook> visible;
      if (!VisibleBooks_(account, visible))
         return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");

      AnsiString bookFilter = QueryParameter_(query, "book");
      std::vector<VisibleBook> books;
      if (bookFilter.IsEmpty())
      {
         // The account's own books. With no book made this is the one default
         // book, which is every contact it has - what this route has always
         // answered.
         for (size_t i = 0; i < visible.size(); i++)
         {
            if (visible[i].mine)
               books.push_back(visible[i]);
         }
      }
      else if (bookFilter == "all")
      {
         books = visible;
      }
      else
      {
         __int64 wanted = _atoi64(bookFilter.c_str());
         for (size_t i = 0; i < visible.size(); i++)
         {
            if (visible[i].id == wanted && wanted > 0)
               books.push_back(visible[i]);
         }
         if (books.empty())
            return BuildResponse_(404, "{\"error\":\"no such address book\"}");
      }

      // More than one book in the answer means every contact says which it is
      // in; one book means the page already knows.
      const bool label = books.size() > 1 || bookFilter == "all";

      AnsiString json = "{\"contacts\":[";
      int count = 0;
      int total = 0;

      // One group index, for the READER'S OWN address book, built at most once
      // however many of its books this listing walks. There is never one for
      // anybody else's, because a group of a book somebody shares with the
      // reader is not listed at all: a group is one account's own entry and
      // resolves against that account's WHOLE address book - every book of it,
      // shared or not - so listing one to a sharee would answer with the names
      // and addresses of contacts in books that were never shared. That is why
      // a group card is also refused on a CardDAV PUT into somebody else's
      // book: a writable share would otherwise be a way to ask for the owner's
      // private rows by planting a group that names them.
      std::shared_ptr<ContactGroupIndex> index;

      for (size_t b = 0; b < books.size(); b++)
      {
         const VisibleBook &book = books[b];

         std::vector<ContactRecord> contacts;
         if (!ContactStore::ListInBook(book.ownerAccountId, book.id, book.isDefault, contacts))
            return BuildResponse_(500, "{\"error\":\"the contacts could not be read\"}");

         for (const ContactRecord &contact : contacts)
         {
            const bool group = ContactGroups::IsGroup(contact);
            if (!kindFilter.IsEmpty() && (kindFilter == "group") != group)
               continue;

            // A group row in somebody else's book is theirs, not the reader's.
            if (group && !book.mine)
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

            if (!matches)
               continue;

            total++;
            if (count >= limit)
               continue;

            if (count > 0)
               json += ",";

            if (group)
            {
               if (!index)
               {
                  String readerAddress = account->GetAddress();
                  readerAddress.ToLower();

                  std::vector<ContactRecord> all;
                  ContactStore::List(account->GetID(), all);
                  index = std::shared_ptr<ContactGroupIndex>(new ContactGroupIndex(Utf8_(readerAddress), all));
               }

               ContactGroup resolved;
               ContactGroups::Resolve(*index, contact, resolved);
               json += ContactGroupJson_(resolved, &contact, includeCard, label ? &book : nullptr);
            }
            else
               json += ContactJson_(contact, includeCard, label ? &book : nullptr);

            count++;
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

      // Which book it goes in: the one the body names, or the account's own
      // default. A book shared read-only refuses it here as CardDAV does.
      // 0 is the account's default book everywhere in this feature - it is what
      // every contact carried before schema 6079 - so a client that sends it
      // means the default book rather than a book that is not there.
      __int64 wanted = 0;
      const bool named = requestBody.Find("\"book\"") >= 0 && JsonWholeNumber_(requestBody, "book", wanted) >= 0;
      if (requestBody.Find("\"book\"") >= 0 && !named)
         return BuildResponse_(400, "{\"error\":\"book must be an address book's id\"}");

      VisibleBook book;
      if (named && wanted > 0)
      {
         if (!FindVisibleBook_(account, wanted, book))
            return BuildResponse_(404, "{\"error\":\"no such address book\"}");
         if (!book.writable)
            return BuildResponse_(403, "{\"error\":\"this address book is shared with you read-only\"}");
      }
      else
      {
         AddressBookRecord defaultBook;
         if (!AddressBookStore::EnsureDefault(account->GetID(), defaultBook))
            return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");
         if (!FindVisibleBook_(account, defaultBook.id, book))
            return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");
      }

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

      // The address is unique per ACCOUNT rather than per book, so the row that
      // already has it may be in another book of the owner's.
      __int64 existing = 0;
      if (ContactStore::FindByAddress(book.ownerAccountId, address, existing))
         return BuildResponse_(409, "{\"error\":\"a contact with that address exists\",\"id\":" + Int64Text(existing) + "}");

      ContactRecord inserted;
      bool ok = card.IsEmpty()
         ? ContactStore::Insert(book.ownerAccountId, book.id, name, address, ContactStore::SourceManual, inserted)
         : ContactStore::InsertCard(book.ownerAccountId, book.id, name, address, String(), uid, card, inserted);
      if (!ok)
         return BuildResponse_(500, "{\"error\":\"the contact could not be saved\"}");

      return BuildResponse_(201, ContactJson_(inserted, false, &book));
   }

   HttpResponse
   RestApiServer::HandleMeContactUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The row must be in a book the account can see and may write to; one it
      // cannot see at all is 404, not 403, so that the ids of one address book
      // say nothing about another.
      ContactRecord contact;
      VisibleBook book;
      int status = 404;
      AnsiString problem;
      if (!FindVisibleContact_(account, id, true, contact, book, status, problem))
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      const __int64 ownerId = book.ownerAccountId;

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
         if (ContactStore::FindByAddress(ownerId, address, other) && other != id)
            return BuildResponse_(409, "{\"error\":\"a contact with that address exists\",\"id\":" + Int64Text(other) + "}");
      }

      // book: moved from one address book to another. Both must belong to the
      // same account - the row is that account's, and moving it into somebody
      // else's book would be giving away a contact rather than moving one - and
      // the destination must be writable.
      VisibleBook destination = book;
      if (requestBody.Find("\"book\"") >= 0)
      {
         __int64 wanted = 0;
         if (JsonWholeNumber_(requestBody, "book", wanted) < 0)
            return BuildResponse_(400, "{\"error\":\"book must be an address book's id\"}");

         // 0 is the account's default book, as it is everywhere else here.
         if (wanted <= 0)
         {
            AddressBookRecord defaultBook;
            if (!AddressBookStore::EnsureDefault(account->GetID(), defaultBook))
               return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");
            wanted = defaultBook.id;
         }

         if (!FindVisibleBook_(account, wanted, destination))
            return BuildResponse_(404, "{\"error\":\"no such address book\"}");
         if (destination.ownerAccountId != ownerId)
            return BuildResponse_(400, "{\"error\":\"a contact is moved between the books of one account; add it to the other book instead\"}");
         if (!destination.writable)
            return BuildResponse_(403, "{\"error\":\"this address book is shared with you read-only\"}");
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

      if (!ContactStore::Update(ownerId, id, name, address))
         return BuildResponse_(500, "{\"error\":\"the contact could not be saved\"}");

      if (source != contact.source && !ContactStore::SetSource(ownerId, id, source))
         return BuildResponse_(500, "{\"error\":\"the contact could not be kept\"}");

      if (destination.id != book.id && !ContactStore::SetBook(ownerId, id, destination.id))
         return BuildResponse_(500, "{\"error\":\"the contact could not be moved\"}");

      ContactRecord saved;
      if (!ContactStore::Get(ownerId, id, saved))
         return BuildResponse_(500, "{\"error\":\"the contact could not be read back\"}");

      return BuildResponse_(200, ContactJson_(saved, false, &destination));
   }

   HttpResponse
   RestApiServer::HandleMeContactDelete_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      ContactRecord contact;
      VisibleBook book;
      int status = 404;
      AnsiString problem;
      if (!FindVisibleContact_(account, id, true, contact, book, status, problem))
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      if (!ContactStore::Delete(book.ownerAccountId, id))
         return BuildResponse_(500, "{\"error\":\"the contact could not be deleted\"}");

      // The groups that named it - a contact or a group in a group - no
      // longer do (ContactGroups.h). The OWNER's groups, since the row was
      // theirs.
      std::shared_ptr<const Account> owner = book.mine ? account : CacheContainer::Instance()->GetAccount(book.ownerAccountId);
      if (owner)
         ContactGroups::ForgetMember(owner, contact);

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
      VisibleBook book;
      int status = 404;
      AnsiString problem;
      if (!FindVisibleContact_(account, id, false, contact, book, status, problem))
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AnsiString bytes;
      AnsiString contentType;
      if (!ContactCard::Photo(contact.vcard, bytes, contentType))
         return BuildResponse_(404, "{\"error\":\"the contact has no photo\"}");

      HttpResponse response;
      response.status = 200;
      // The type is the card's, which whoever wrote the card chose - and a
      // card can be in a book another account shares: a picture on the list
      // of those no browser runs or renders (InertMediaType.h), or
      // application/octet-stream.
      response.content_type = InertMediaType::ForPicture(contentType);
      response.body = bytes;
      response.extra_headers =
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: private, max-age=3600\r\n";
      return response;
   }
}
