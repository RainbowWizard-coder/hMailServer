// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's address books, and who they are shared with:
// GET and POST /api/v1/me/address-books, PUT and DELETE on one of them, and
// GET, PUT and DELETE under its /shares. The store is hm_addressbooks and
// hm_addressbookshares (schema 6079, AddressBookStore.h); the contacts in a
// book are rows of hm_contacts and belong to the book's OWNER whoever is
// reading them.
//
// An administrator sees the shape of the sharing and nothing else:
// GET /api/v1/accounts/{address}/address-books lists an account's books, how
// many contacts each holds and who each is shared with, and DELETE under a
// book's /shares takes one back. There is deliberately no route here that
// reads the contacts of somebody's address book: an administrator who needs
// that has the account export and the support session, both of which say what
// they are and are recorded.
//
// A share names one other account of this server, or a whole domain. Both are
// written in the path as they are read: someone@example.com is an account,
// example.com is a domain. Sharing with one's own account is refused, and so is
// sharing a book with a domain that is not hosted here - there would be nobody
// to give it to.
//
// A DOMAIN share is the sharer's OWN domain and no other. Every account of the
// domain gets the book whether it wanted it or not and cannot leave it, since
// the share was not made to it; what makes that bearable is that the domain's
// administrator can remove the share, and the route that does so is scoped to
// the book owner's domain. Aimed at somebody else's domain the same share would
// be an unleavable book in every one of their Contacts pages that neither they
// nor their own administrator could remove. An account of another domain is
// shared with by address, and can leave.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "AddressBookStore.h"
#include "ContactStore.h"
#include "ContactGroups.h"
#include "Unicode.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "AuditTrail.h"

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

      String Lowered(const AnsiString &value)
      {
         AnsiString trimmed = value;
         trimmed.TrimLeft();
         trimmed.TrimRight();

         String wide;
         Unicode::MultiByteToWide(trimmed, wide);
         wide.ToLower();
         return wide;
      }

      // The target of a share: an address names an account, anything else a
      // domain. Answers the ids, or false with the reason.
      bool ResolveShareTarget(const AnsiString &target, __int64 &accountId, __int64 &domainId, AnsiString &problem)
      {
         accountId = 0;
         domainId = 0;

         String wanted = Lowered(target);
         if (wanted.IsEmpty())
         {
            problem = "the share names an account of this server or one of its domains";
            return false;
         }

         if (wanted.Find(_T("@")) >= 0)
         {
            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(wanted);
            if (!account)
            {
               problem = "no account on this server has that address";
               return false;
            }
            accountId = account->GetID();
            return true;
         }

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(wanted);
         if (!domain)
         {
            problem = "this server hosts no domain of that name";
            return false;
         }
         domainId = domain->GetID();
         return true;
      }

      // How many contacts a book holds, groups and collected addresses
      // included: what the listing shows beside its name. Counted without
      // reading the cards, since this is asked once per book. False when the
      // table could not be read, which the caller answers 500 for rather than
      // printing 0: "contacts":0 is the statement that the book is empty.
      bool ContactCount(const RestApiServer::VisibleBook &book, int &count)
      {
         count = 0;
         return ContactStore::CountInBook(book.ownerAccountId, book.id, book.isDefault, count);
      }
   }

   bool
   RestApiServer::VisibleBooks_(std::shared_ptr<const Account> account, std::vector<VisibleBook> &books)
   {
      books.clear();
      if (!account)
         return false;

      String ownAddress = account->GetAddress();
      ownAddress.ToLower();

      std::vector<AddressBookRecord> own;
      if (!AddressBookStore::ListBooks(account->GetID(), own))
         return false;

      for (size_t i = 0; i < own.size(); i++)
      {
         VisibleBook book;
         book.id = own[i].id;
         book.ownerAccountId = own[i].accountId;
         book.created = own[i].created;
         book.name = own[i].name;
         book.owner = ownAddress;
         book.mine = true;
         book.writable = true;
         book.isDefault = i == 0;
         books.push_back(book);
      }

      // A failed read of hm_addressbookshares is a failure of the whole
      // listing, not an answer of "nobody shares anything with you": the books
      // other people share are part of what this account can see, and a list
      // with them silently missing reads exactly like a share that was taken
      // back. Every caller answers 500 for it.
      std::vector<AddressBookRecord> shared;
      std::vector<AddressBookShareRecord> shares;
      if (!AddressBookStore::ListSharedWith(account->GetID(), account->GetDomainID(), shared, shares))
      {
         books.clear();
         return false;
      }

      for (size_t i = 0; i < shared.size(); i++)
      {
         std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(shared[i].accountId);
         String ownerAddress = owner ? owner->GetAddress() : String();
         ownerAddress.ToLower();

         VisibleBook book;
         book.id = shared[i].id;
         book.ownerAccountId = shared[i].accountId;
         book.created = shared[i].created;
         book.name = shared[i].name;
         book.owner = ownerAddress;
         book.mine = false;
         book.writable = shares[i].write;
         book.isDefault = AddressBookStore::IsDefault(shared[i].accountId, shared[i].id);
         books.push_back(book);
      }

      return true;
   }

   bool
   RestApiServer::FindVisibleBook_(std::shared_ptr<const Account> account, __int64 bookId, VisibleBook &found)
   {
      std::vector<VisibleBook> books;
      if (!VisibleBooks_(account, books))
         return false;

      for (size_t i = 0; i < books.size(); i++)
      {
         if (books[i].id == bookId)
         {
            found = books[i];
            return true;
         }
      }

      return false;
   }

   bool
   RestApiServer::FindVisibleContact_(std::shared_ptr<const Account> account, __int64 id, bool needWrite,
                                      ContactRecord &contact, VisibleBook &book, int &status, AnsiString &problem)
   {
      status = 404;
      problem = "no such contact";

      if (!account)
      {
         status = 500;
         problem = "internal error";
         return false;
      }

      ContactRecord found;
      if (!ContactStore::GetAny(id, found))
         return false;

      // A GROUP is never shared. It is one account's own address book entry,
      // it expands against that account's whole address book - across all of
      // its books - and a row of it that happens to sit in a book its owner
      // shared is still not the sharee's to read, change or delete. A group
      // row of another account is therefore not found at all, which is the
      // same 404 a contact of a book nobody shared gets.
      const bool group = ContactStore::IsGroupAddress(found.address);
      if (group && found.accountId != account->GetID())
         return false;

      std::vector<VisibleBook> books;
      if (!VisibleBooks_(account, books))
      {
         status = 500;
         problem = "the address books could not be read";
         return false;
      }

      for (size_t i = 0; i < books.size(); i++)
      {
         if (books[i].ownerAccountId != found.accountId)
            continue;

         // The default book also holds the rows an upgrade left at 0.
         const bool inBook = found.bookId == books[i].id || (books[i].isDefault && found.bookId == 0);
         if (!inBook)
            continue;

         if (needWrite && !books[i].writable)
         {
            status = 403;
            problem = "this address book is shared with you read-only";
            return false;
         }

         contact = found;
         book = books[i];
         return true;
      }

      // A row of another account, in a book nobody shared: 404 rather than 403,
      // so the ids of one address book say nothing about another's.
      return false;
   }

   AnsiString
   RestApiServer::OpenApiAddressBookPaths_()
   {
      // The comma is this block's, as it is every block's: the document is
      // the blocks joined in RestApiServer.cpp, and without it this one
      // followed the contact groups' closing brace as `}"/api/`, which is not
      // JSON - the full gate of 18 September 2026 found it in six fixtures.
      return
         ",\"/api/v1/me/address-books\":{"
         "\"get\":{\"summary\":\"The address books the signed-in account can see\",\"description\":\"Its own, the default first, then the ones other accounts share with it or with its whole domain. Each: id, name, owner, mine, writable, is_default, created, contacts (how many), and for its own books shares - each target (an address or a domain name), kind (account or domain) and write.\",\"responses\":{\"200\":{\"description\":\"books\"}}},"
         "\"post\":{\"summary\":\"Make an address book\",\"description\":\"Body: name. At most 50 books per account, and two of one account cannot share a name.\",\"responses\":{\"201\":{\"description\":\"The book\"},\"400\":{\"description\":\"The name is missing, too long or unusable\"},\"409\":{\"description\":\"A book of that name exists\"},\"507\":{\"description\":\"The account already has the 50 books it may\"}}}},"
         "\"/api/v1/me/address-books/{id}\":{"
         "\"put\":{\"summary\":\"Rename an address book\",\"description\":\"Body: name. Only the owner renames one; the default book can be renamed like any other.\",\"responses\":{\"200\":{\"description\":\"The book\"},\"403\":{\"description\":\"Somebody else's book\"},\"404\":{\"description\":\"No such book\"},\"409\":{\"description\":\"A book of that name exists\"}}},"
         "\"delete\":{\"summary\":\"Delete an address book, or leave one shared with you\",\"description\":\"Your own book goes with every contact in it, and with the shares of it; the default book cannot be deleted. A book somebody shares with you is left rather than deleted - the share is given back and nothing of theirs is touched - and a book shared with your whole domain cannot be left, since the share is not yours to take back.\",\"responses\":{\"200\":{\"description\":\"deleted or left\"},\"403\":{\"description\":\"The default book, or a domain-wide share\"},\"404\":{\"description\":\"No such book\"}}}},"
         "\"/api/v1/me/address-books/{id}/shares\":{"
         "\"get\":{\"summary\":\"Who an address book is shared with\",\"description\":\"Only the owner asks. Each: target, kind (account or domain), write, created.\",\"responses\":{\"200\":{\"description\":\"shares\"},\"403\":{\"description\":\"Somebody else's book\"},\"404\":{\"description\":\"No such book\"}}}},"
         "\"/api/v1/me/address-books/{id}/shares/{target}\":{"
         "\"put\":{\"summary\":\"Share an address book, or change what a share allows\",\"description\":\"target is an account's address, or a domain name for every account of that domain - which must be the sharer's own domain, since a domain-wide share is not one its recipients can leave. Body: write (false by default, which is read-only). Only the owner shares, and never with itself.\",\"responses\":{\"200\":{\"description\":\"The book with its shares\"},\"400\":{\"description\":\"The target is the owner, or is neither an address nor a domain name\"},\"403\":{\"description\":\"Somebody else's book, or a domain that is not the sharer's own\"},\"404\":{\"description\":\"No such book, account or domain\"},\"507\":{\"description\":\"The book already has the 200 shares it may\"}}},"
         "\"delete\":{\"summary\":\"Take a share back\",\"description\":\"The book and its contacts stay where they are; the person or the domain simply stops seeing them, in the webmail and over CardDAV.\",\"responses\":{\"200\":{\"description\":\"The book with its shares\"},\"403\":{\"description\":\"Somebody else's book\"},\"404\":{\"description\":\"No such book or share\"}}}},"
         "\"/api/v1/accounts/{address}/address-books\":{"
         "\"get\":{\"summary\":\"An account's address books and who they are shared with (administrator)\",\"description\":\"id, name, is_default, created, contacts (how many) and shares - target, kind and write - for each book the account owns. This read makes nothing: an account that has never asked for its books answers the one default book at id 0, which is what its contacts carry until it does. The contacts themselves are not here: an administrator who needs them has the account export and the support session, both of which say what they are.\",\"responses\":{\"200\":{\"description\":\"books\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/address-books/{id}/shares/{target}\":{"
         "\"delete\":{\"summary\":\"Remove a share of an account's address book (administrator)\",\"description\":\"What an operator does when somebody has shared a team's address book with an account that should not have it. Recorded in the audit trail as address-book-share-removed, with the book and the target.\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"No such account, book or share\"}}}}";
   }

   namespace
   {
      AnsiString ShareJson(const AddressBookShareRecord &share)
      {
         return AnsiString("{\"target\":\"") + RestApiServer::JsonEscapeUtf8(share.target) +
            "\",\"kind\":\"" + (share.domainId > 0 ? "domain" : "account") +
            "\",\"write\":" + (share.write ? "true" : "false") +
            ",\"created\":" + Int64Text(share.created) + "}";
      }

      // Who a book is shared with. False when the table could not be read:
      // "shares":[] says the book is shared with nobody, which is the opposite
      // of what a failed read knows, and this listing is where an operator
      // looks to find out who has somebody's address book.
      bool SharesJson(__int64 bookId, AnsiString &json)
      {
         std::vector<AddressBookShareRecord> shares;
         if (!AddressBookStore::ListShares(bookId, shares))
            return false;

         json = "[";
         for (size_t i = 0; i < shares.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += ShareJson(shares[i]);
         }
         json += "]";
         return true;
      }

      // A book, with what it holds and - for one of the caller's own - who it
      // is shared with. False when either of those could not be read, so that
      // no count and no share list is printed as a fact that a query failed to
      // establish.
      bool BookJson(const RestApiServer::VisibleBook &book, bool withShares, AnsiString &json)
      {
         int contacts = 0;
         if (!ContactCount(book, contacts))
            return false;

         json = AnsiString("{\"id\":") + Int64Text(book.id) +
            ",\"name\":\"" + RestApiServer::JsonEscapeUtf8(book.name) +
            "\",\"owner\":\"" + RestApiServer::JsonEscapeUtf8(book.owner) +
            "\",\"mine\":" + (book.mine ? "true" : "false") +
            ",\"writable\":" + (book.writable ? "true" : "false") +
            ",\"is_default\":" + (book.isDefault && book.mine ? "true" : "false") +
            ",\"created\":" + Int64Text(book.created) +
            ",\"contacts\":" + Int64Text(contacts);

         if (withShares && book.mine)
         {
            // A book that is not written down yet (id 0, the default one the
            // account has not asked for) has no row, so it has no shares
            // either; asking for them would be a query for a book id nothing
            // holds.
            AnsiString shares = "[]";
            if (book.id > 0 && !SharesJson(book.id, shares))
               return false;
            json += ",\"shares\":" + shares;
         }

         json += "}";
         return true;
      }
   }

   HttpResponse
   RestApiServer::HandleMeAddressBooks_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::vector<VisibleBook> books;
      if (!VisibleBooks_(account, books))
         return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");

      AnsiString json = "{\"books\":[";
      for (size_t i = 0; i < books.size(); i++)
      {
         AnsiString one;
         if (!BookJson(books[i], true, one))
            return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");
         if (i > 0)
            json += ",";
         json += one;
      }
      json += "],\"count\":" + Int64Text((__int64) books.size()) + "}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      String name = JsonUtf8Value_(requestBody, "name");
      name.TrimLeft();
      name.TrimRight();
      if (name.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name is required\"}");
      if (!AddressBookStore::IsUsableName(name))
         return BuildResponse_(400, "{\"error\":\"name is 1 to 255 printable characters\"}");

      std::vector<VisibleBook> books;
      if (!VisibleBooks_(account, books))
         return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");

      int owned = 0;
      for (size_t i = 0; i < books.size(); i++)
      {
         if (!books[i].mine)
            continue;
         owned++;
         if (books[i].name == name)
            return BuildResponse_(409, "{\"error\":\"an address book of that name exists\",\"id\":" + Int64Text(books[i].id) + "}");
      }

      if (owned >= AddressBookStore::MaximumBooks)
      {
         AnsiString most;
         most.Format("%d", AddressBookStore::MaximumBooks);
         return BuildResponse_(507, "{\"error\":\"this account already holds the " + most + " address books it may; delete one before making another\"}");
      }

      AddressBookRecord created;
      if (!AddressBookStore::CreateBook(account->GetID(), name, created))
         return BuildResponse_(500, "{\"error\":\"the address book could not be made\"}");

      VisibleBook book;
      AnsiString json;
      if (!FindVisibleBook_(account, created.id, book) || !BookJson(book, true, json))
         return BuildResponse_(500, "{\"error\":\"the address book could not be read back\"}");

      return BuildResponse_(201, json);
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookUpdate_(const Caller &caller, __int64 bookId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      VisibleBook book;
      if (!FindVisibleBook_(account, bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");
      if (!book.mine)
         return BuildResponse_(403, "{\"error\":\"the address book is another account's; only its owner names it\"}");

      String name = book.name;
      if (requestBody.Find("\"name\"") >= 0)
      {
         name = JsonUtf8Value_(requestBody, "name");
         name.TrimLeft();
         name.TrimRight();
         if (!AddressBookStore::IsUsableName(name))
            return BuildResponse_(400, "{\"error\":\"name is 1 to 255 printable characters\"}");
      }

      AddressBookRecord taken;
      if (AddressBookStore::FindByName(account->GetID(), name, taken) && taken.id != bookId)
         return BuildResponse_(409, "{\"error\":\"an address book of that name exists\",\"id\":" + Int64Text(taken.id) + "}");

      if (!AddressBookStore::RenameBook(account->GetID(), bookId, name))
         return BuildResponse_(500, "{\"error\":\"the address book could not be saved\"}");

      VisibleBook after;
      AnsiString json;
      if (!FindVisibleBook_(account, bookId, after) || !BookJson(after, true, json))
         return BuildResponse_(500, "{\"error\":\"the address book could not be read back\"}");
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookDelete_(const Caller &caller, __int64 bookId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      VisibleBook book;
      if (!FindVisibleBook_(account, bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");

      if (!book.mine)
      {
         // Leaving a book somebody shared. Only the share made to this account:
         // one made to the whole domain is not this person's to take back.
         AddressBookShareRecord own;
         if (!AddressBookStore::ShareOf(bookId, account->GetID(), 0, own))
            return BuildResponse_(403, "{\"error\":\"this address book is shared with your whole domain; only its owner can take that share back\"}");

         if (!AddressBookStore::Unshare(bookId, account->GetID(), 0))
            return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");

         return BuildResponse_(200, "{\"left\":true}");
      }

      if (book.isDefault)
         return BuildResponse_(403, "{\"error\":\"the default address book cannot be deleted\"}");

      // The groups of this account that named a contact of the book no longer
      // name it: the rows are about to go, and a group card left pointing at a
      // row that is not there is the shape the group index reads as a member
      // another client deleted.
      std::vector<ContactRecord> going;
      if (ContactStore::ListInBook(account->GetID(), bookId, false, going))
      {
         for (size_t i = 0; i < going.size(); i++)
            ContactGroups::ForgetMember(account, going[i]);
      }

      if (!AddressBookStore::DeleteBook(account->GetID(), bookId))
         return BuildResponse_(500, "{\"error\":\"the address book could not be deleted\"}");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookShares_(const Caller &caller, __int64 bookId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      VisibleBook book;
      if (!FindVisibleBook_(account, bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");
      if (!book.mine)
         return BuildResponse_(403, "{\"error\":\"the address book is another account's; only its owner sees who it is shared with\"}");

      AnsiString shares;
      if (!SharesJson(bookId, shares))
         return BuildResponse_(500, "{\"error\":\"the shares could not be read\"}");
      return BuildResponse_(200, "{\"shares\":" + shares + "}");
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookSharePut_(const Caller &caller, __int64 bookId, const AnsiString &target, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      VisibleBook book;
      if (!FindVisibleBook_(account, bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");
      if (!book.mine)
         return BuildResponse_(403, "{\"error\":\"only the address book's owner shares it\"}");

      __int64 shareAccountId = 0;
      __int64 shareDomainId = 0;
      AnsiString problem;
      if (!ResolveShareTarget(target, shareAccountId, shareDomainId, problem))
         return BuildResponse_(problem.Find("no ") == 0 || problem.Find("this server hosts") == 0 ? 404 : 400,
            "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      if (shareAccountId == account->GetID())
         return BuildResponse_(400, "{\"error\":\"an address book is already its owner's\"}");

      // A domain-wide share reaches every account of the domain at once, none
      // of whom asked for it and none of whom can take it back - leaving one
      // is refused below, because the share is not theirs. That is bearable
      // for the sharer's OWN domain, whose administrator can remove the share
      // on /api/v1/accounts/{address}/address-books/{id}/shares/{target}, and
      // it is not bearable for anyone else's: it would let any account of any
      // hosted domain push a book into every Contacts page and every To
      // completion of another domain, with the receiving domain's own
      // administrator unable to remove it, since that route is scoped to the
      // OWNER'S domain. A share with one account of another domain stays
      // allowed: that account can leave it.
      if (shareDomainId > 0 && shareDomainId != account->GetDomainID())
         return BuildResponse_(403, "{\"error\":\"an address book is shared with a whole domain only when it is your own domain; share it with the accounts of another domain by address, which they can then leave\"}");

      std::vector<AddressBookShareRecord> existing;
      if (!AddressBookStore::ListShares(bookId, existing))
         return BuildResponse_(500, "{\"error\":\"the shares could not be read\"}");

      AddressBookShareRecord already;
      if (!AddressBookStore::ShareOf(bookId, shareAccountId, shareDomainId, already) &&
          (int) existing.size() >= AddressBookStore::MaximumShares)
      {
         AnsiString most;
         most.Format("%d", AddressBookStore::MaximumShares);
         return BuildResponse_(507, "{\"error\":\"this address book already has the " + most + " shares it may\"}");
      }

      const bool write = GetJsonBoolValue_(requestBody, "write", false);
      if (!AddressBookStore::Share(bookId, shareAccountId, shareDomainId, write))
         return BuildResponse_(500, "{\"error\":\"the share could not be saved\"}");

      LOG_DEBUG("Address books: " + account->GetAddress() + " shared " + book.name + " with " + Lowered(target) +
         (write ? _T(" (read and write).") : _T(" (read-only).")));

      VisibleBook after;
      AnsiString json;
      if (!FindVisibleBook_(account, bookId, after) || !BookJson(after, true, json))
         return BuildResponse_(500, "{\"error\":\"the address book could not be read back\"}");
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeAddressBookShareDelete_(const Caller &caller, __int64 bookId, const AnsiString &target)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      VisibleBook book;
      if (!FindVisibleBook_(account, bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");
      if (!book.mine)
         return BuildResponse_(403, "{\"error\":\"only the address book's owner shares it\"}");

      __int64 shareAccountId = 0;
      __int64 shareDomainId = 0;
      AnsiString problem;
      if (!ResolveShareTarget(target, shareAccountId, shareDomainId, problem))
         return BuildResponse_(404, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AddressBookShareRecord share;
      if (!AddressBookStore::ShareOf(bookId, shareAccountId, shareDomainId, share))
         return BuildResponse_(404, "{\"error\":\"the address book is not shared with that target\"}");

      if (!AddressBookStore::Unshare(bookId, shareAccountId, shareDomainId))
         return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");

      VisibleBook after;
      AnsiString json;
      if (!FindVisibleBook_(account, bookId, after) || !BookJson(after, true, json))
         return BuildResponse_(500, "{\"error\":\"the address book could not be read back\"}");
      return BuildResponse_(200, json);
   }

   //------------------------------------------------------------------------
   // The administrator's two
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleAccountAddressBooks_(const AnsiString &address)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(Lowered(address));
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      // ListBooksReadOnly, not ListBooks: this route needs ViewAccounts and
      // nothing more, which is a permission the auditor role holds, and
      // ListBooks would make the default book and stamp the account's contacts
      // with its id - an insert and an unbounded update run by a read that
      // check-role-choke-point.py has classified as changing nothing. An
      // account nobody has asked for its books yet is reported as the one
      // default book it will make when somebody does, at id 0, which is what
      // its contacts already carry.
      std::vector<AddressBookRecord> own;
      if (!AddressBookStore::ListBooksReadOnly(account->GetID(), own))
         return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");

      if (own.empty())
      {
         AddressBookRecord waiting;
         waiting.accountId = account->GetID();
         waiting.name = String(AddressBookStore::DefaultName);
         own.push_back(waiting);
      }

      String ownAddress = account->GetAddress();
      ownAddress.ToLower();

      AnsiString json = "{\"address\":\"" + JsonEscapeUtf8(ownAddress) + "\",\"books\":[";
      for (size_t i = 0; i < own.size(); i++)
      {
         VisibleBook book;
         book.id = own[i].id;
         book.ownerAccountId = own[i].accountId;
         book.created = own[i].created;
         book.name = own[i].name;
         book.owner = ownAddress;
         book.mine = true;
         book.writable = true;
         book.isDefault = i == 0;

         AnsiString one;
         if (!BookJson(book, true, one))
            return BuildResponse_(500, "{\"error\":\"the address books could not be read\"}");

         if (i > 0)
            json += ",";
         json += one;
      }
      json += "],\"count\":" + Int64Text((__int64) own.size()) + "}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleAccountAddressBookShareDelete_(const AnsiString &address, __int64 bookId, const AnsiString &target)
   {
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(Lowered(address));
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      AddressBookRecord book;
      if (!AddressBookStore::Get(account->GetID(), bookId, book))
         return BuildResponse_(404, "{\"error\":\"no such address book\"}");

      __int64 shareAccountId = 0;
      __int64 shareDomainId = 0;
      AnsiString problem;
      if (!ResolveShareTarget(target, shareAccountId, shareDomainId, problem))
         return BuildResponse_(404, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AddressBookShareRecord share;
      if (!AddressBookStore::ShareOf(bookId, shareAccountId, shareDomainId, share))
         return BuildResponse_(404, "{\"error\":\"the address book is not shared with that target\"}");

      if (!AddressBookStore::Unshare(bookId, shareAccountId, shareDomainId))
         return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");

      // The rows of hm_addressbookshares are the account's own and are not
      // audited as statements; a share an administrator takes back is somebody
      // else's decision undone, so it is recorded as the event it is.
      AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("address-book-share-removed"),
         String(_T("The address book \"")) + book.name + _T("\" is no longer shared with ") + Lowered(target) + _T("."));

      return BuildResponse_(200, "{\"removed\":true}");
   }
}
