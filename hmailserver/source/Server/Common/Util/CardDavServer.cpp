// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See CardDavServer.h for the shape of the tree. This file is the request
// side of it: a small namespace-aware XML reader for the PROPFIND and REPORT
// bodies (XMLite is neither namespace-aware nor meant for input off the wire),
// the DAV: and CARDDAV: property set of each resource kind, the three reports,
// and PUT/GET/DELETE of one contact as a vCard, on top of ContactStore and
// VCard.
//
// The bits of RFC 4918, 6352 and 6578 that shape the answers:
//
//   - PROPFIND (4918 section 9.1) answers 207 with one D:response per resource
//     in scope, and inside it one D:propstat for the properties found and one,
//     status 404, for those asked for and not there. Depth: 0 is the resource,
//     1 adds its members; infinity is treated as 1, as sabre/dav does, rather
//     than refused, since nothing here is deeper than one level anyway.
//   - The reports (6352 section 8, 6578 section 3): addressbook-multiget takes
//     hrefs and answers each, 404 for one that is not in this address book;
//     addressbook-query takes a filter of prop-filters with text-matches and
//     answers the contacts that match; sync-collection takes a token. The
//     store keeps no change log, so a token is the state of the whole
//     collection: an empty token lists everything, the current token answers
//     "no changes", and any other is refused with DAV:valid-sync-token so the
//     client lists again. That is a valid answer under 6578 section 3.2 and it
//     keeps the cheap poll cheap; it is not an incremental sync.
//   - ETags (6352 section 6.3.2.3) are strong and are the SHA-256 of the card
//     the server serves, so they change exactly when the card does. getctag
//     and the sync-token are the SHA-256 of every member's id and ETag.
//   - PUT with If-None-Match: * creates and is 412 on an existing resource;
//     If-Match must equal the current ETag or the answer is 412 (7232). A card
//     with no EMAIL is 403 with CARDDAV:valid-address-data and a sentence
//     saying why: this address book keeps a name and one e-mail address per
//     contact, and a card without one has nothing to be stored as.
//
// SEVERAL ADDRESS BOOKS, AND SHARED ONES (schema 6079, AddressBookStore.h). The
// home collection holds one collection per book the account can see:
//
//   contacts/     the account's default book, at the URL it has always been at,
//                 so a client that has been syncing since 6.3 keeps syncing.
//   book-<id>/    another book of the account's own.
//   shared-<id>/  a book another account shares with this one, by name or with
//                 the whole domain.
//
// A shared collection's contacts are the OWNER'S rows: everything under it is
// read and written under the owner's account id, and the sharee's own id never
// reaches a row of it. A book shared read-only refuses PUT and DELETE with 403
// and says so in current-user-privilege-set, so a client is told before it
// tries. DELETE on a shared collection gives the share back and leaves the book
// where it is - exactly what the calendar does.

#include "StdAfx.h"

#include <algorithm>
#include <map>
#include <set>
#include <openssl/sha.h>

#include "CardDavServer.h"
#include "DavSupport.h"
#include "CalDavServer.h"
#include "ContactStore.h"
#include "AddressBookStore.h"
#include "ContactGroups.h"
#include "DirectoryStore.h"
#include "../Application/Application.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"
#include "VCard.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const char *CardDavServer::ContextPath = "/dav/";

   using namespace Dav;

   namespace
   {
      const char *NsDav = "DAV:";
      const char *NsCardDav = "urn:ietf:params:xml:ns:carddav";
      const char *NsCalendarServer = "http://calendarserver.org/ns/";
      const char *NsCalDav = "urn:ietf:params:xml:ns:caldav";

      // How many hrefs one multiget may name; the XML ceilings are DavSupport's.
      const size_t MaxMultigetHrefs = 5000;

      // What CARDDAV:max-resource-size advertises, and the cap PUT enforces
      // on a card: the listener's large-request cap, less nothing - a card
      // that arrives has fitted already.
      const int MaxCardBytes = 1024 * 1024;

      const char *AllowContact = "Allow: OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, REPORT\r\n";
      // DELETE is here because a collection somebody shares with the caller can
      // be left with one (HandleDelete); on the caller's own books it answers
      // 403 with the reason, which is a refusal rather than a method nobody
      // has ever heard of.
      const char *AllowCollection = "Allow: OPTIONS, PROPFIND, PROPPATCH, REPORT, DELETE\r\n";

      // The directory is read-only, and says so in the one header a client
      // reads to find out: no PUT, no DELETE, no PROPPATCH.
      const char *AllowDirectoryCard = "Allow: OPTIONS, GET, HEAD, PROPFIND, REPORT\r\n";
      const char *AllowDirectoryCollection = "Allow: OPTIONS, PROPFIND, REPORT\r\n";

      //------------------------------------------------------------------------
      // The resources
      //------------------------------------------------------------------------

      enum ResourceKind
      {
         KindRoot,                // /dav/
         KindPrincipalCollection, // /dav/principals/
         KindPrincipal,           // /dav/principals/<address>/
         KindHomeCollection,      // /dav/addressbooks/
         KindCalendarsCollection, // /dav/calendars/ - CalDavServer's tree, listed here so the root is complete
         KindHome,                // /dav/addressbooks/<address>/
         KindAddressBook,         // /dav/addressbooks/<address>/<book>/ - contacts, book-<id> or shared-<id>
         KindContact,             // .../<book>/<id>.vcf, and the contact exists
         KindContactSlot,         // .../<book>/<name>, nothing there yet: PUT may create
         KindDirectory,           // /dav/addressbooks/<address>/directory/ - the organisation's, read-only
         KindDirectoryCard,       // .../directory/account-<id>.vcf or list-<id>.vcf
         KindDirectorySlot        // .../directory/<name>, nothing there: a PUT is refused, not 404
      };

      // The three kinds that are the organisation's directory rather than one
      // of the account's address books (DirectoryStore.h).
      bool IsDirectoryKind(ResourceKind kind)
      {
         return kind == KindDirectory || kind == KindDirectoryCard || kind == KindDirectorySlot;
      }

      // One address book as this request sees it: the row, whose it is, what
      // the caller may do in it, and where it lives in the tree.
      struct Book
      {
         Book() : shared(false), writable(true), isDefault(false), ownerAccountId(0) { }

         AddressBookRecord record;
         bool shared;             // somebody else's, shared with the caller
         bool writable;           // the caller may add, change and delete in it
         bool isDefault;          // the owner's default book, served at contacts/
         __int64 ownerAccountId;  // whose rows the contacts in it are
         AnsiString ownerUtf8;    // and that account's address
         AnsiString segment;      // "contacts", "book-<id>" or "shared-<id>"
         AnsiString href;         // the collection's canonical href, with its slash
      };

      struct Resource
      {
         Resource() : kind(KindRoot) { }

         ResourceKind kind;
         AnsiString href;        // the canonical href, percent-encoded
         Book book;              // KindAddressBook, KindContact and KindContactSlot
         ContactRecord contact;  // KindContact only
         AnsiString slotName;    // KindContactSlot only: the name, decoded
         int directoryIndex = -1;   // KindDirectoryCard only: its place in the loaded directory

         // KindPrincipal of a room or a piece of equipment of the caller's own
         // domain rather than the caller's own principal (ResourceBooking.h):
         // its address, its name and what it is. A room is meant to be found,
         // as the directory lists it; a person's principal stays the person's.
         bool otherPrincipal = false;
         AnsiString principalAddressUtf8;
         AnsiString principalName;
         int principalKind = 0;
      };

      // A member of an address book as the responses see it: the row, the
      // card the server serves for it and that card's ETag.
      //
      // A directory member carries its entry as well, and its card is built
      // without the PHOTO until something asks for the card's body (CardOf).
      // The photo is up to 137 KB of base64 per person and the collection is
      // polled for its ctag by every phone that subscribes to it; building
      // every card with every photo to answer "has anything changed" was the
      // whole domain's photographs per poll. The ETag is computed so that it
      // does not need the photo either - see FillDirectoryMember.
      struct Member
      {
         ContactRecord record;
         mutable AnsiString card;
         AnsiString etag;
         AnsiString href;
         AnsiString uid;

         DirectoryEntry entry;             // the directory's members only
         mutable bool photoPending = false;
      };

      // The card of a member, with its photo if it has one that has not been
      // read yet. The lazy read is the only thing mutable here: a Context
      // belongs to one request on one thread.
      const AnsiString &CardOf(const Member &member)
      {
         if (!member.photoPending)
            return member.card;

         member.photoPending = false;

         DirectoryRecord whole;
         if (DirectoryStore::Get(member.entry.id, whole) && whole.stored && !whole.photo.IsEmpty())
         {
            // The opt-out is the listing's - it is what let this entry be
            // listed at all - and the rest of the row is read again with the
            // photo, which is the only thing that was missing.
            DirectoryEntry complete = member.entry;
            complete.record = whole;
            complete.record.optOut = member.entry.record.optOut;
            member.card = DirectoryStore::GenerateCard(member.uid, complete);
         }

         return member.card;
      }

      // Everything one request needs to know, computed once.
      struct Context
      {
         Context(const HttpRequest &httpRequest) : request(httpRequest), booksLoaded(false), booksFailed(false) { }

         const HttpRequest &request;
         std::shared_ptr<const Account> account;

         String addressWide;         // the account's address, lower-cased
         AnsiString addressUtf8;
         AnsiString principalHref;
         AnsiString homeHref;
         AnsiString directoryHref;
         __int64 domainId = 0;

         // Every book the caller can see - its own and the shared ones - loaded
         // on first need, and the members of each loaded on first need after
         // that. A PROPFIND of the home collection asks for all of them; a
         // request inside one book asks for that one.
         bool booksLoaded;
         bool booksFailed;
         std::vector<Book> books;
         std::map<__int64, std::vector<Member> > membersByBook;
         std::set<__int64> failedBooks;

         // The organisation's directory, loaded on first need, beside the
         // books: a PROPFIND on the home asks every collection for its ctag in
         // one request, and the directory's members are nobody's book.
         bool directoryListed = false;
         bool directoryFailed = false;
         std::vector<Member> directoryMembers;
      };

      // The domain part of an address, for the sentence the directory's
      // description is.
      String DomainPartOf(const String &address)
      {
         const int at = address.Find(_T("@"));
         return at < 0 ? String() : address.Mid(at + 1);
      }

      // A name-based UUID of a seed, marked as version 8 (RFC 9562 section 5.8)
      // because its bits come from SHA-256: the same seed gives the same UID.
      AnsiString StableUidFromSeed_(const AnsiString &seed);

      // The UID of a contact the webmail made, which has no card and so no UID
      // of its own: derived from the account's address and the row id, so the
      // same row gives the same UID for as long as the account keeps its
      // address. A card a client sent carries its own UID, kept in the row.
      AnsiString StableUid_(const AnsiString &addressUtf8, __int64 id)
      {
         return StableUidFromSeed_("hMailServer.carddav.contact:" + addressUtf8 + ":" + Int64Text(id));
      }

      AnsiString StableUidFromSeed_(const AnsiString &seed)
      {

         unsigned char digest[SHA256_DIGEST_LENGTH] = {};
         SHA256(reinterpret_cast<const unsigned char *>(seed.c_str()), static_cast<size_t>(seed.GetLength()), digest);
         digest[6] = static_cast<unsigned char>((digest[6] & 0x0f) | 0x80);
         digest[8] = static_cast<unsigned char>((digest[8] & 0x3f) | 0x80);

         static const char *hex = "0123456789abcdef";
         AnsiString result;
         for (int i = 0; i < 16; i++)
         {
            if (i == 4 || i == 6 || i == 8 || i == 10)
               result += "-";
            result += hex[digest[i] >> 4];
            result += hex[digest[i] & 0x0f];
         }
         return result;
      }

      // The resource name a client created the contact under, or <id>.vcf for
      // a contact the webmail made, under the collection of the book it is in.
      AnsiString ContactHref(const Book &book, const ContactRecord &record)
      {
         if (record.uri.IsEmpty())
            return book.href + Int64Text(record.id) + ".vcf";
         return book.href + PercentEncodeSegment(Utf8(record.uri));
      }

      void FillMember(const Book &book, const ContactRecord &record, Member &member)
      {
         member.record = record;

         // The UID is derived from the address of the account whose row it is -
         // the book's OWNER - so a contact of a shared book has one identity
         // however many people read it.
         member.uid = record.uid.IsEmpty() ? StableUid_(book.ownerUtf8, record.id) : Utf8(record.uid);

         // A card a client sent is served as it came, byte for byte, so the
         // ETag the client was given for it is the ETag of what it reads back
         // (RFC 9110 section 8.8.3); a row that never came from a client - the
         // webmail's - is served as a card made from its name and address. A
         // group always has a card (ContactGroups.h); one that lost it is
         // served as an empty group rather than as a contact at its marker.
         if (!record.vcard.IsEmpty())
            member.card = Utf8(record.vcard);
         else if (ContactGroups::IsGroup(record))
            member.card = VCard::GenerateGroup(member.uid, Utf8(record.name), std::vector<AnsiString>());
         else
            member.card = VCard::Generate(member.uid, Utf8(record.name), Utf8(record.address));
         member.etag = EtagOf(member.card);
         member.href = ContactHref(book, record);
      }

      // The resource name a directory entry is served under. It is derived from
      // the row id and not from the address, so that a person whose address
      // changes keeps their place in a phone's sync rather than appearing to be
      // deleted and created; the UID inside the card is derived from the
      // address, which is what a client matches a card by.
      AnsiString DirectoryHref(const Context &context, const DirectoryEntry &entry)
      {
         return context.directoryHref + (entry.list ? "list-" : "account-") + Int64Text(entry.id) + ".vcf";
      }

      void FillDirectoryMember(const Context &context, const DirectoryEntry &entry, Member &member)
      {
         // The directory has no rows of hm_contacts behind it. What a Member
         // needs of one is an id, a name and an address, and those are the
         // entry's; nothing writes it back.
         member.record = ContactRecord();
         member.record.id = entry.id;
         member.record.name = entry.name;
         member.record.address = entry.address;

         member.entry = entry;
         member.uid = DirectoryStore::StableUid(Utf8(entry.address), entry.list);
         member.card = DirectoryStore::GenerateCard(member.uid, entry);
         member.href = DirectoryHref(context, entry);

         // The photo is not in the card yet, and the ETag must not depend on
         // it: the same member is answered from a listing that has not read
         // any photo and from a GET that has read this one, and a client that
         // saw two ETags for one unchanged card would fetch it for ever. So
         // the tag is taken over the card WITHOUT the photo, plus the two
         // things the listing knows about it - whether there is one, and when
         // the row was last written. Change the photo and dirupdated moves,
         // so the tag moves with it.
         AnsiString tagged = member.card;
         tagged += "PHOTO-STATE:" + (entry.hasPhoto ? Utf8(entry.record.photoType) : AnsiString("")) + ":" + Utf8(entry.record.updated) + "\r\n";
         member.etag = EtagOf(tagged);

         member.photoPending = !entry.list && entry.hasPhoto;
      }

      // Whether a row of the owner's address book is in this book. The default
      // book also holds the rows an upgrade left at 0 (ContactStore.h).
      bool InBook(const Book &book, const ContactRecord &record)
      {
         return record.bookId == book.record.id || (book.isDefault && record.bookId == 0);
      }

      // And whether this collection serves it. A GROUP row is its owner's own
      // address book entry: its members are resolved against the owner's WHOLE
      // address book, across every book of it, so a group of a book somebody
      // shares with the caller would carry names and addresses out of books
      // that were never shared. No group is served through a share, in either
      // direction - a group card PUT into a shared book is refused below for
      // the same reason.
      bool ServedIn(const Book &book, const ContactRecord &record)
      {
         return !book.shared || !ContactStore::IsGroupAddress(record.address);
      }

      // The collection for one of the caller's own books, filled in.
      void FillOwnBook(const Context &context, const AddressBookRecord &record, bool isDefault, Book &book)
      {
         book = Book();
         book.record = record;
         book.shared = false;
         book.writable = true;
         book.isDefault = isDefault;
         book.ownerAccountId = record.accountId;
         book.ownerUtf8 = context.addressUtf8;
         book.segment = isDefault ? AnsiString("contacts") : "book-" + Int64Text(record.id);
         book.href = context.homeHref + book.segment + "/";
      }

      // And for one somebody else shares with the caller.
      void FillSharedBook(const Context &context, const AddressBookRecord &record, bool writable, Book &book)
      {
         book = Book();
         book.record = record;
         book.shared = true;
         book.writable = writable;
         book.isDefault = AddressBookStore::IsDefault(record.accountId, record.id);
         book.ownerAccountId = record.accountId;

         std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(record.accountId);
         String ownerAddress = owner ? owner->GetAddress() : String();
         ownerAddress.ToLower();
         book.ownerUtf8 = Utf8(ownerAddress);

         book.segment = "shared-" + Int64Text(record.id);
         book.href = context.homeHref + book.segment + "/";
      }

      // Every book the caller can see: its own, default first, then the ones
      // shared with it by name or through its domain.
      bool EnsureBooks(Context &context)
      {
         if (context.booksLoaded)
            return !context.booksFailed;

         context.booksLoaded = true;

         std::vector<AddressBookRecord> own;
         if (!AddressBookStore::ListBooks(context.account->GetID(), own))
         {
            context.booksFailed = true;
            return false;
         }

         for (size_t i = 0; i < own.size(); i++)
         {
            Book book;
            FillOwnBook(context, own[i], i == 0, book);
            context.books.push_back(book);
         }

         // A failed read of hm_addressbookshares fails the whole listing, as a
         // failed read of the caller's own books does. Swallowed, it would
         // leave the shared collections out of the tree, Resolve would not find
         // the one a client asked for, and the answer would be 404 - which is
         // the client being told its address book was deleted, and a client
         // told that throws its copy away. booksFailed turns it into the 500
         // the request is retried after.
         std::vector<AddressBookRecord> shared;
         std::vector<AddressBookShareRecord> shares;
         if (!AddressBookStore::ListSharedWith(context.account->GetID(), context.account->GetDomainID(), shared, shares))
         {
            context.books.clear();
            context.booksFailed = true;
            return false;
         }

         for (size_t i = 0; i < shared.size(); i++)
         {
            Book book;
            FillSharedBook(context, shared[i], shares[i].write, book);
            context.books.push_back(book);
         }

         return true;
      }

      // The members of one book, read once per request however many properties
      // ask for them. Null when the book could not be read.
      const std::vector<Member> *MembersOf(Context &context, const Book &book)
      {
         std::map<__int64, std::vector<Member> >::const_iterator found = context.membersByBook.find(book.record.id);
         if (found != context.membersByBook.end())
            return &found->second;

         if (context.failedBooks.find(book.record.id) != context.failedBooks.end())
            return nullptr;

         std::vector<ContactRecord> records;
         if (!ContactStore::ListInBook(book.ownerAccountId, book.record.id, book.isDefault, records))
         {
            context.failedBooks.insert(book.record.id);
            return nullptr;
         }

         std::vector<Member> &members = context.membersByBook[book.record.id];
         for (size_t i = 0; i < records.size(); i++)
         {
            // A collected address - somebody the person wrote to - is not a
            // contact on the phone until the person keeps it.
            if (records[i].source == ContactStore::SourceCollected)
               continue;

            // And a group of a book somebody shares with the caller is the
            // owner's, not theirs.
            if (!ServedIn(book, records[i]))
               continue;

            Member member;
            FillMember(book, records[i], member);
            members.push_back(member);
         }

         return &members;
      }

      // The organisation's directory of the account's own domain, as cards -
      // two queries, and not one photograph read until a client asks for a
      // card's body. With DirectoryEnabled off the collection is empty rather
      // than absent, so a client that has subscribed to it syncs to nothing
      // instead of failing every poll.
      bool EnsureDirectoryListed(Context &context)
      {
         if (context.directoryListed)
            return !context.directoryFailed;

         context.directoryListed = true;

         if (!DirectoryStore::Enabled())
            return true;

         std::vector<DirectoryEntry> entries;
         if (!DirectoryStore::List(context.domainId, entries))
         {
            context.directoryFailed = true;
            return false;
         }

         for (size_t i = 0; i < entries.size(); i++)
         {
            Member member;
            FillDirectoryMember(context, entries[i], member);
            context.directoryMembers.push_back(member);
         }

         return true;
      }

      // The directory's members, or null when it could not be read: the shape
      // MembersOf answers for a book, so that the reports treat the two alike.
      const std::vector<Member> *DirectoryMembers(Context &context)
      {
         return EnsureDirectoryListed(context) ? &context.directoryMembers : nullptr;
      }

      // The members a resource's collection serves: the directory's, or those
      // of the book the resource is in.
      const std::vector<Member> *MembersFor(Context &context, const Resource &resource)
      {
         return IsDirectoryKind(resource.kind) ? DirectoryMembers(context) : MembersOf(context, resource.book);
      }

      // The state of a whole collection, from which getctag and the
      // sync-token are made: every member's id and ETag, in id order.
      bool TagOfMembers(const std::vector<Member> &members, AnsiString &tag)
      {
         std::vector<AnsiString> lines;
         for (size_t i = 0; i < members.size(); i++)
            lines.push_back(Int64Text(members[i].record.id) + ":" + members[i].etag);
         std::sort(lines.begin(), lines.end());

         AnsiString all;
         for (size_t i = 0; i < lines.size(); i++)
            all += lines[i] + "\n";

         tag = Sha256Hex(all).Left(32);
         return true;
      }

      bool CollectionTag(Context &context, const Book &book, AnsiString &tag)
      {
         const std::vector<Member> *members = MembersOf(context, book);
         return members ? TagOfMembers(*members, tag) : false;
      }

      // The tag of whichever collection the resource is: the directory's, or
      // its book's.
      bool CollectionTagFor(Context &context, const Resource &resource, AnsiString &tag)
      {
         const std::vector<Member> *members = MembersFor(context, resource);
         return members ? TagOfMembers(*members, tag) : false;
      }

      AnsiString SyncTokenFor(const AnsiString &tag)
      {
         return "urn:x-hmailserver:carddav-sync:" + tag;
      }

      // Where a contact the caller can see lives in the tree, whichever book
      // holds it. A 409 names the row that already has an address, and the
      // address is unique per account rather than per book, so that row may be
      // in another of the account's books; one in a book the caller cannot see
      // has no href here, and the refusal names the address instead.
      AnsiString HrefOfVisibleContact(Context &context, const ContactRecord &record)
      {
         if (!EnsureBooks(context))
            return AnsiString("");

         for (size_t i = 0; i < context.books.size(); i++)
         {
            if (context.books[i].ownerAccountId == record.accountId && InBook(context.books[i], record))
               return ContactHref(context.books[i], record);
         }

         return AnsiString("");
      }

      // How a 409 names that row: at its URL when the caller can see where it
      // is, and not at all when it is in a book they cannot - an empty pair of
      // brackets said nothing and read like a bug.
      AnsiString At(const AnsiString &href)
      {
         return href.IsEmpty() ? AnsiString("") : " (" + href + ")";
      }

      // RFC 6638 section 2.4.2: what a calendar user is. A person is an
      // INDIVIDUAL, a room a ROOM and a piece of equipment a RESOURCE.
      AnsiString CalendarUserTypeOf(int kind)
      {
         if (kind == Account::KindRoom)
            return "ROOM";
         if (kind == Account::KindEquipment)
            return "RESOURCE";
         return "INDIVIDUAL";
      }

      void FillResourcePrincipal(const AnsiString &addressUtf8, const AnsiString &nameUtf8, int kind, Resource &resource)
      {
         resource.kind = KindPrincipal;
         resource.otherPrincipal = true;
         resource.principalAddressUtf8 = addressUtf8;
         resource.principalName = nameUtf8;
         resource.principalKind = kind;
         resource.href = AnsiString(CardDavServer::ContextPath) + "principals/" + PercentEncodeSegment(addressUtf8) + "/";
      }

      bool SameAddress(const Context &context, const AnsiString &segment)
      {
         String wide = Wide(segment);
         wide.ToLower();
         return wide == context.addressWide;
      }

      // The resource a path names, or false: a path outside the tree, or into
      // another account's part of it, is simply not found. Another account's
      // principal is 404 rather than 403 so that the tree of one account says
      // nothing about the existence of another.
      //
      // readsCards is false for the methods that cannot succeed inside the
      // directory whatever the name means - PUT, DELETE, PROPPATCH, OPTIONS,
      // MKCOL - and then a name under the collection is resolved without
      // listing it: a card and a slot are answered exactly alike by all of
      // them, so listing the domain to tell them apart is work done for an
      // answer that does not depend on it.
      bool Resolve(Context &context, const AnsiString &target, Resource &resource, bool readsCards)
      {
         std::vector<AnsiString> segments = PathSegments(target);

         // "/dav" or "/dav/" -> ["dav"] or ["dav", ""]
         if (segments.empty() || segments[0] != "dav")
            return false;

         // Drop the trailing empty segment a trailing slash leaves.
         bool trailingSlash = segments.size() > 1 && segments.back().IsEmpty();
         if (trailingSlash)
            segments.pop_back();

         if (segments.size() == 1)
         {
            resource.kind = KindRoot;
            resource.href = CardDavServer::ContextPath;
            return true;
         }

         if (segments[1] == "principals")
         {
            if (segments.size() == 2)
            {
               resource.kind = KindPrincipalCollection;
               resource.href = AnsiString(CardDavServer::ContextPath) + "principals/";
               return true;
            }
            if (segments.size() == 3 && SameAddress(context, segments[2]))
            {
               resource.kind = KindPrincipal;
               resource.href = context.principalHref;
               return true;
            }
            if (segments.size() == 3 && DirectoryStore::Enabled())
            {
               std::shared_ptr<const Account> other = CacheContainer::Instance()->GetAccount(Wide(segments[2]));
               if (other && other->IsResource() && other->GetActive() && other->GetDomainID() == context.domainId)
               {
                  String address = other->GetAddress();
                  address.ToLower();
                  FillResourcePrincipal(Utf8(address), Utf8(DirectoryStore::NameOf(other->GetPersonFirstName(), other->GetPersonLastName())),
                                        other->GetKind(), resource);
                  return true;
               }
            }
            return false;
         }

         if (segments[1] == "addressbooks")
         {
            if (segments.size() == 2)
            {
               resource.kind = KindHomeCollection;
               resource.href = AnsiString(CardDavServer::ContextPath) + "addressbooks/";
               return true;
            }
            if (!SameAddress(context, segments[2]))
               return false;
            if (segments.size() == 3)
            {
               resource.kind = KindHome;
               resource.href = context.homeHref;
               return true;
            }
            if (segments[3] == "directory")
            {
               if (segments.size() == 4)
               {
                  resource.kind = KindDirectory;
                  resource.href = context.directoryHref;
                  return true;
               }

               if (segments.size() != 5 || trailingSlash || segments[4].IsEmpty())
                  return false;

               // A method that cannot succeed here is answered the same for a
               // card and for a name the directory does not serve, so it is
               // not worth a listing to tell them apart.
               if (!readsCards)
               {
                  resource.kind = KindDirectorySlot;
                  resource.href = context.directoryHref + PercentEncodeSegment(segments[4]);
                  resource.slotName = segments[4];
                  return true;
               }

               // Nothing here is a slot for a reader: the directory is the
               // server's to write and a name it does not serve is simply not
               // there.
               if (!EnsureDirectoryListed(context))
                  return false;

               const AnsiString wanted = context.directoryHref + PercentEncodeSegment(segments[4]);
               const std::vector<Member> &members = context.directoryMembers;
               for (size_t i = 0; i < members.size(); i++)
               {
                  if (members[i].href != wanted)
                     continue;

                  resource.kind = KindDirectoryCard;
                  resource.contact = members[i].record;
                  resource.href = members[i].href;
                  resource.directoryIndex = (int) i;
                  return true;
               }

               // A name the directory does not serve. It is a slot rather
               // than nothing, so that a client that tries to write into the
               // collection is told it is read-only instead of being told the
               // resource does not exist - which would invite it to try again.
               resource.kind = KindDirectorySlot;
               resource.href = context.directoryHref + PercentEncodeSegment(segments[4]);
               resource.slotName = segments[4];
               return true;
            }

            // One of the collections of the home: contacts, book-<id> or
            // shared-<id>. A segment naming a book the caller cannot see is
            // simply not found, so the ids of one account's books say nothing
            // about another's.
            if (!EnsureBooks(context))
               return false;

            const Book *book = nullptr;
            for (size_t i = 0; i < context.books.size() && !book; i++)
            {
               if (context.books[i].segment == segments[3])
                  book = &context.books[i];
            }

            if (!book)
               return false;

            resource.book = *book;

            if (segments.size() == 4)
            {
               resource.kind = KindAddressBook;
               resource.href = book->href;
               return true;
            }
            if (segments.size() != 5 || trailingSlash || segments[4].IsEmpty())
               return false;

            // The name a client created a contact under names that contact;
            // <id>.vcf names a contact the webmail made, which has no name of
            // its own; any other name is a slot a PUT may fill. Both are asked
            // within this book, under the account whose rows it holds.
            AnsiString name = segments[4];
            ContactRecord named;
            const bool byUri = ContactStore::FindByUri(book->ownerAccountId, book->record.id, book->isDefault, Wide(name), named) &&
                               named.source != ContactStore::SourceCollected;
            if (byUri && ServedIn(*book, named))
            {
               resource.kind = KindContact;
               resource.contact = named;
               resource.href = ContactHref(*book, named);
               return true;
            }

            // A row this collection does not serve - a group of a book somebody
            // shares with the caller - is not found, and is not a free slot
            // either: a PUT into its name would leave two rows of the book at
            // one resource name, which the store does not forbid and a client
            // could not tell apart.
            if (byUri)
               return false;

            if (name.EndsWith(".vcf"))
            {
               AnsiString idText = name.Mid(0, name.GetLength() - 4);
               bool numeric = !idText.IsEmpty() && idText.GetLength() < 19;
               for (int i = 0; numeric && i < idText.GetLength(); i++)
                  numeric = idText[i] >= '0' && idText[i] <= '9';

               if (numeric)
               {
                  __int64 id = 0;
                  for (int i = 0; i < idText.GetLength(); i++)
                     id = id * 10 + (idText[i] - '0');

                  ContactRecord record;
                  if (id > 0 && ContactStore::Get(book->ownerAccountId, id, record) && InBook(*book, record) &&
                      record.uri.IsEmpty() && record.source != ContactStore::SourceCollected)
                  {
                     // The same rule, and for the same reason: <id>.vcf is the
                     // name that row is served under, so it is never a slot.
                     if (!ServedIn(*book, record))
                        return false;

                     resource.kind = KindContact;
                     resource.contact = record;
                     resource.href = ContactHref(*book, record);
                     return true;
                  }
               }
            }

            resource.kind = KindContactSlot;
            resource.href = book->href + PercentEncodeSegment(name);
            resource.slotName = name;
            return true;
         }

         return false;
      }

      //------------------------------------------------------------------------
      // Responses
      //------------------------------------------------------------------------

      // A DAV:error body (RFC 4918 section 16) naming the precondition that
      // failed, with a sentence for the person reading the client's log.
      HttpResponse DavError(int status, const AnsiString &preconditionXml, const AnsiString &description)
      {
         AnsiString xml = "<D:error xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:carddav\">" + preconditionXml +
            "<D:responsedescription>" + XmlEscape(description) + "</D:responsedescription></D:error>\r\n";
         return Xml(status, xml);
      }

      HttpResponse MethodNotAllowed(const Resource &resource)
      {
         if (resource.kind == KindDirectoryCard || resource.kind == KindDirectorySlot)
            return Text(405, "method not allowed", AllowDirectoryCard);
         if (resource.kind == KindDirectory)
            return Text(405, "method not allowed", AllowDirectoryCollection);

         bool contact = resource.kind == KindContact || resource.kind == KindContactSlot;
         return Text(405, "method not allowed", contact ? AllowContact : AllowCollection);
      }

      AnsiString MultistatusOpen()
      {
         return "<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:carddav\" xmlns:CAL=\"urn:ietf:params:xml:ns:caldav\" xmlns:CS=\"http://calendarserver.org/ns/\">\r\n";
      }

      //------------------------------------------------------------------------
      // Properties
      //------------------------------------------------------------------------

      struct PropertyName
      {
         PropertyName() { }
         PropertyName(const char *propertyNs, const char *propertyName) : ns(propertyNs), name(propertyName) { }

         AnsiString ns;
         AnsiString name;
      };

      // How the properties are asked for.
      enum PropertyMode
      {
         ModeProp,      // the ones named
         ModeAllProp,   // every one the resource has (but not address-data)
         ModePropName   // the names of every one, no values
      };

      struct PropertyRequest
      {
         PropertyRequest() : mode(ModeAllProp) { }

         PropertyMode mode;
         std::vector<PropertyName> names;

         // address-data's partial-retrieval element, if the request named
         // properties inside it: the card is then reduced to those.
         std::vector<AnsiString> addressDataProperties;
      };

      // The element for a property in a response: prefixed for the three
      // namespaces the document declares, a default-namespace element for any
      // other. Empty inner XML makes an empty element.
      AnsiString PropertyElement(const PropertyName &property, const AnsiString &innerXml)
      {
         AnsiString prefix;
         if (property.ns == NsDav) prefix = "D:";
         else if (property.ns == NsCardDav) prefix = "C:";
         else if (property.ns == NsCalDav) prefix = "CAL:";
         else if (property.ns == NsCalendarServer) prefix = "CS:";

         AnsiString open = "<" + prefix + property.name;
         if (prefix.IsEmpty())
            open += " xmlns=\"" + XmlEscape(property.ns) + "\"";

         if (innerXml.IsEmpty())
            return open + "/>";
         return open + ">" + innerXml + "</" + prefix + property.name + ">";
      }

      AnsiString Href(const AnsiString &href)
      {
         return "<D:href>" + XmlEscape(href) + "</D:href>";
      }

      // The card reduced to the properties a partial address-data asked for.
      AnsiString ReduceCard(const AnsiString &card, const std::vector<AnsiString> &wanted)
      {
         if (wanted.empty())
            return card;

         AnsiString result;
         std::vector<AnsiString> lines = StringParser::SplitString(card, "\r\n");
         for (size_t i = 0; i < lines.size(); i++)
         {
            const AnsiString &line = lines[i];
            if (line.IsEmpty())
               continue;

            int colon = line.Find(":");
            int semicolon = line.Find(";");
            int end = colon;
            if (semicolon >= 0 && semicolon < end)
               end = semicolon;
            AnsiString name = end > 0 ? line.Mid(0, end) : line;
            name.ToUpper();

            bool keep = name == "BEGIN" || name == "END" || name == "VERSION";
            for (size_t j = 0; !keep && j < wanted.size(); j++)
               keep = wanted[j].CompareNoCase(name) == 0;

            if (keep)
               result += line + "\r\n";
         }
         return result;
      }

      // The value of one live property of a resource, or false when the
      // resource has no such property.
      bool PropertyValue(Context &context, const Resource &resource, const Member *member, const PropertyRequest &request,
                         const PropertyName &property, AnsiString &innerXml)
      {
         ResourceKind kind = resource.kind;
         bool isContact = kind == KindContact || kind == KindDirectoryCard;
         bool isCollection = !isContact && kind != KindContactSlot && kind != KindDirectorySlot;
         const bool directory = IsDirectoryKind(kind);
         const bool anyBook = kind == KindAddressBook || kind == KindDirectory;

         if (property.ns == NsDav)
         {
            if (property.name == "resourcetype")
            {
               innerXml = "";
               if (isCollection)
                  innerXml += "<D:collection/>";
               if (kind == KindPrincipal)
                  innerXml += "<D:principal/>";
               if (anyBook)
                  innerXml += "<C:addressbook/>";
               return true;
            }

            if (property.name == "displayname")
            {
               switch (kind)
               {
               case KindRoot: innerXml = "hMailServer"; break;
               case KindPrincipalCollection: innerXml = "Principals"; break;
               case KindPrincipal:
                  innerXml = XmlEscape(!resource.otherPrincipal ? context.addressUtf8
                     : (resource.principalName.IsEmpty() ? resource.principalAddressUtf8 : resource.principalName));
                  break;
               case KindHomeCollection: innerXml = "Address books"; break;
               case KindCalendarsCollection: innerXml = "Calendars"; break;
               case KindHome: innerXml = XmlEscape("Address books of " + context.addressUtf8); break;
               case KindDirectory: innerXml = "Directory"; break;
               case KindAddressBook:
                  innerXml = XmlEscape(Utf8(resource.book.record.name) +
                     (resource.book.shared ? AnsiString(" (") + resource.book.ownerUtf8 + ")" : AnsiString("")));
                  break;
               case KindContact:
               case KindDirectoryCard:
                  innerXml = XmlEscape(resource.contact.name.IsEmpty() ? Utf8(resource.contact.address) : Utf8(resource.contact.name));
                  break;
               default: return false;
               }
               return true;
            }

            if (property.name == "current-user-principal")
            {
               innerXml = Href(context.principalHref);
               return true;
            }

            if (property.name == "principal-collection-set")
            {
               innerXml = Href(AnsiString(CardDavServer::ContextPath) + "principals/");
               return true;
            }

            if (property.name == "principal-URL" && kind == KindPrincipal)
            {
               innerXml = Href(resource.otherPrincipal ? resource.href : context.principalHref);
               return true;
            }

            if (property.name == "owner" && (kind == KindHome || anyBook || isContact))
            {
               // A shared collection's owner is the account whose rows it holds,
               // not the one reading it.
               if (kind != KindHome && resource.book.shared)
                  innerXml = Href(AnsiString(CardDavServer::ContextPath) + "principals/" + PercentEncodeSegment(resource.book.ownerUtf8) + "/");
               else
                  innerXml = Href(context.principalHref);
               return true;
            }

            if (property.name == "current-user-privilege-set" && (kind == KindPrincipal || kind == KindHome || anyBook || isContact))
            {
               // A book shared read-only grants reading and nothing else, so a
               // client is told before it tries to write; everything the caller
               // owns, or is shared with write, is writable. The directory is
               // the organisation's, kept by the administrator and by each
               // person's own settings, so a phone is told plainly that it may
               // read it and nothing else - rather than being allowed to try a
               // PUT and told 403 at the end of it.
               const bool writable = directory || resource.otherPrincipal ? false : (kind == KindPrincipal || kind == KindHome) ? true : resource.book.writable;

               innerXml = "<D:privilege><D:read/></D:privilege>"
                          "<D:privilege><D:read-current-user-privilege-set/></D:privilege>";
               if (writable)
                  innerXml += "<D:privilege><D:write/></D:privilege>"
                              "<D:privilege><D:write-content/></D:privilege><D:privilege><D:bind/></D:privilege>"
                              "<D:privilege><D:unbind/></D:privilege>";
               return true;
            }

            if (property.name == "supported-report-set" && anyBook)
            {
               innerXml = "<D:supported-report><D:report><C:addressbook-multiget/></D:report></D:supported-report>"
                          "<D:supported-report><D:report><C:addressbook-query/></D:report></D:supported-report>"
                          "<D:supported-report><D:report><D:sync-collection/></D:report></D:supported-report>";
               return true;
            }

            if (property.name == "sync-token" && anyBook)
            {
               AnsiString tag;
               if (!CollectionTagFor(context, resource, tag))
                  return false;
               innerXml = XmlEscape(SyncTokenFor(tag));
               return true;
            }

            if (property.name == "getetag" && isContact && member)
            {
               innerXml = XmlEscape(member->etag);
               return true;
            }

            if (property.name == "getcontenttype" && isContact)
            {
               innerXml = "text/vcard; charset=utf-8";
               return true;
            }

            if (property.name == "getcontentlength" && isContact && member)
            {
               // CardOf and not member->card: the length answered must be the
               // length of what a GET would serve, photo and all.
               innerXml = IntText(CardOf(*member).GetLength());
               return true;
            }

            return false;
         }

         if (property.ns == NsCardDav)
         {
            if (property.name == "addressbook-home-set" && (kind == KindRoot || (kind == KindPrincipal && !resource.otherPrincipal)))
            {
               innerXml = Href(context.homeHref);
               return true;
            }

            if (anyBook)
            {
               if (property.name == "supported-address-data")
               {
                  // Both versions are accepted on PUT and a card is served back as it
                  // was sent, so both are advertised - 3.0 first, being what a client
                  // sends unless told otherwise.
                  innerXml = "<C:address-data-type content-type=\"text/vcard\" version=\"3.0\"/>"
                             "<C:address-data-type content-type=\"text/vcard\" version=\"4.0\"/>";
                  return true;
               }
               if (property.name == "addressbook-description")
               {
                  innerXml = directory
                     ? XmlEscape("The people and lists of " + Utf8(DomainPartOf(context.addressWide)) + ", read-only")
                     : XmlEscape(Utf8(resource.book.record.name) + " of " + resource.book.ownerUtf8 +
                          (resource.book.shared
                             ? AnsiString(resource.book.writable ? ", shared with you" : ", shared with you read-only")
                             : AnsiString("")));
                  return true;
               }
               if (property.name == "max-resource-size")
               {
                  innerXml = IntText(MaxCardBytes);
                  return true;
               }
               if (property.name == "supported-collation-set")
               {
                  innerXml = "<C:supported-collation>i;ascii-casemap</C:supported-collation>"
                             "<C:supported-collation>i;octet</C:supported-collation>"
                             "<C:supported-collation>i;unicode-casemap</C:supported-collation>";
                  return true;
               }
            }

            if (property.name == "address-data" && isContact && member)
            {
               // A client that asks for the cards themselves is asking for the
               // photographs in them; this is the one place a directory card's
               // photo is read, and only for the cards actually asked for.
               innerXml = XmlEscape(ReduceCard(CardOf(*member), request.addressDataProperties));
               return true;
            }

            return false;
         }

         if (property.ns == NsCalDav)
         {
            // The principal is one for both protocols: a calendar client
            // that walks from /dav/ finds its calendar home here, served by
            // CalDavServer.
            if (property.name == "calendar-home-set" && (kind == KindRoot || (kind == KindPrincipal && !resource.otherPrincipal)))
            {
               innerXml = Href(CalDavServer::HomeHref(context.addressUtf8));
               return true;
            }
            if (property.name == "calendar-user-address-set" && kind == KindPrincipal)
            {
               innerXml = Href("mailto:" + (resource.otherPrincipal ? resource.principalAddressUtf8 : context.addressUtf8));
               return true;
            }
            // RFC 6638 section 2.4.2: a room says ROOM, a piece of equipment
            // RESOURCE, a person INDIVIDUAL - what a client's room finder
            // looks for (ResourceBooking.h).
            if (property.name == "calendar-user-type" && kind == KindPrincipal)
            {
               innerXml = CalendarUserTypeOf(resource.otherPrincipal ? resource.principalKind : context.account->GetKind());
               return true;
            }
            return false;
         }

         if (property.ns == NsCalendarServer)
         {
            if (property.name == "getctag" && anyBook)
            {
               AnsiString tag;
               if (!CollectionTagFor(context, resource, tag))
                  return false;
               innerXml = XmlEscape(tag);
               return true;
            }
         }

         return false;
      }

      // Every property a resource kind has, for allprop and propname.
      void KnownProperties(ResourceKind kind, bool includeAddressData, std::vector<PropertyName> &names)
      {
         names.push_back(PropertyName(NsDav, "resourcetype"));
         names.push_back(PropertyName(NsDav, "displayname"));
         names.push_back(PropertyName(NsDav, "current-user-principal"));
         names.push_back(PropertyName(NsDav, "principal-collection-set"));

         switch (kind)
         {
         case KindRoot:
            names.push_back(PropertyName(NsCardDav, "addressbook-home-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-home-set"));
            break;
         case KindPrincipal:
            names.push_back(PropertyName(NsDav, "principal-URL"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            names.push_back(PropertyName(NsCardDav, "addressbook-home-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-home-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-user-address-set"));
            names.push_back(PropertyName(NsCalDav, "calendar-user-type"));
            break;
         case KindHome:
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            break;
         case KindDirectory:
         case KindAddressBook:
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            names.push_back(PropertyName(NsDav, "supported-report-set"));
            names.push_back(PropertyName(NsDav, "sync-token"));
            names.push_back(PropertyName(NsCardDav, "addressbook-description"));
            names.push_back(PropertyName(NsCardDav, "supported-address-data"));
            names.push_back(PropertyName(NsCardDav, "max-resource-size"));
            names.push_back(PropertyName(NsCardDav, "supported-collation-set"));
            names.push_back(PropertyName(NsCalendarServer, "getctag"));
            break;
         case KindDirectoryCard:
         case KindContact:
            names.push_back(PropertyName(NsDav, "getetag"));
            names.push_back(PropertyName(NsDav, "getcontenttype"));
            names.push_back(PropertyName(NsDav, "getcontentlength"));
            names.push_back(PropertyName(NsDav, "owner"));
            names.push_back(PropertyName(NsDav, "current-user-privilege-set"));
            if (includeAddressData)
               names.push_back(PropertyName(NsCardDav, "address-data"));
            break;
         default:
            break;
         }
      }

      // One D:response for a resource: the properties found in a 200
      // propstat, the ones asked for and absent in a 404 propstat.
      AnsiString ResponseFor(Context &context, const Resource &resource, const Member *member, const PropertyRequest &request)
      {
         std::vector<PropertyName> names;
         if (request.mode == ModeProp)
            names = request.names;
         else
         {
            KnownProperties(resource.kind, request.mode == ModePropName, names);

            // allprop with D:include: the named ones as well, once each.
            for (size_t i = 0; i < request.names.size(); i++)
            {
               bool known = false;
               for (size_t j = 0; !known && j < names.size(); j++)
                  known = names[j].ns == request.names[i].ns && names[j].name == request.names[i].name;
               if (!known)
                  names.push_back(request.names[i]);
            }
         }

         AnsiString found;
         AnsiString missing;
         for (size_t i = 0; i < names.size(); i++)
         {
            if (request.mode == ModePropName)
            {
               found += PropertyElement(names[i], "");
               continue;
            }

            AnsiString innerXml;
            if (PropertyValue(context, resource, member, request, names[i], innerXml))
               found += PropertyElement(names[i], innerXml);
            else
               missing += PropertyElement(names[i], "");
         }

         AnsiString xml = " <D:response>\r\n  " + Href(resource.href) + "\r\n";
         if (!found.IsEmpty() || missing.IsEmpty())
            xml += "  <D:propstat><D:prop>" + found + "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>\r\n";
         if (!missing.IsEmpty())
            xml += "  <D:propstat><D:prop>" + missing + "</D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>\r\n";
         xml += " </D:response>\r\n";
         return xml;
      }

      AnsiString ResponseForMember(Context &context, const Book &book, const Member &member, const PropertyRequest &request,
                                   bool directory = false)
      {
         Resource resource;
         resource.kind = directory ? KindDirectoryCard : KindContact;
         resource.book = book;
         resource.contact = member.record;
         resource.href = member.href;
         return ResponseFor(context, resource, &member, request);
      }

      // The D:prop element of a request, into a PropertyRequest.
      void ReadPropertyRequest(const XmlElement *prop, PropertyRequest &request)
      {
         request.mode = ModeProp;
         request.names.clear();
         if (!prop)
            return;

         for (size_t i = 0; i < prop->children.size(); i++)
         {
            const XmlElement &child = prop->children[i];
            PropertyName name;
            name.ns = child.ns;
            name.name = child.name;
            request.names.push_back(name);

            if (child.Is(NsCardDav, "address-data"))
            {
               for (size_t j = 0; j < child.children.size(); j++)
               {
                  if (child.children[j].Is(NsCardDav, "prop"))
                  {
                     AnsiString wanted = child.children[j].Attribute("name");
                     if (!wanted.IsEmpty())
                        request.addressDataProperties.push_back(wanted);
                  }
               }
            }
         }
      }

      //------------------------------------------------------------------------
      // The methods
      //------------------------------------------------------------------------

      bool ReadBody(const HttpRequest &request, XmlElement &root, AnsiString &problem)
      {
         XmlReader reader(request.body);
         return reader.Read(root, problem);
      }

      int DepthOf(const HttpRequest &request)
      {
         AnsiString depth = Lower(Trimmed(request.Header("depth")));
         if (depth == "0")
            return 0;
         // Absent, 1 and infinity: one level. See the file comment.
         return 1;
      }

      HttpResponse HandleOptions(const Resource &resource)
      {
         if (resource.kind == KindDirectoryCard || resource.kind == KindDirectorySlot)
            return Respond(200, "text/plain", "", AllowDirectoryCard);
         if (resource.kind == KindDirectory)
            return Respond(200, "text/plain", "", AllowDirectoryCollection);

         bool contact = resource.kind == KindContact || resource.kind == KindContactSlot;
         return Respond(200, "text/plain", "", contact ? AllowContact : AllowCollection);
      }

      HttpResponse HandlePropfind(Context &context, const Resource &resource)
      {
         if (resource.kind == KindContactSlot || resource.kind == KindDirectorySlot)
            return NotFound();

         PropertyRequest request;
         if (!Trimmed(context.request.body).IsEmpty())
         {
            XmlElement root;
            AnsiString problem;
            if (!ReadBody(context.request, root, problem))
               return Text(400, "the PROPFIND body is not well-formed XML: " + problem);
            if (!root.Is(NsDav, "propfind"))
               return Text(400, "the PROPFIND body must be a DAV:propfind element");

            if (root.Child(NsDav, "propname"))
               request.mode = ModePropName;
            else if (const XmlElement *prop = root.Child(NsDav, "prop"))
               ReadPropertyRequest(prop, request);
            else
            {
               request.mode = ModeAllProp;
               // D:include names properties to add to allprop; they are
               // answered by a second pass over the named ones.
               if (const XmlElement *include = root.Child(NsDav, "include"))
               {
                  PropertyRequest included;
                  ReadPropertyRequest(include, included);
                  request.names = included.names;
               }
            }
         }

         int depth = DepthOf(context.request);

         AnsiString xml = MultistatusOpen();

         // The resource itself, and for allprop with D:include the extra ones.
         const Member *self = nullptr;
         Member selfMember;
         if (resource.kind == KindContact)
         {
            FillMember(resource.book, resource.contact, selfMember);
            self = &selfMember;
         }
         else if (resource.kind == KindDirectoryCard)
         {
            // Resolve found the card in the listing, and its member is there.
            const std::vector<Member> *members = DirectoryMembers(context);
            if (members && resource.directoryIndex >= 0 && (size_t) resource.directoryIndex < members->size())
               self = &(*members)[resource.directoryIndex];
         }
         xml += ResponseFor(context, resource, self, request);

         if (depth == 1)
         {
            switch (resource.kind)
            {
            case KindRoot:
            {
               Resource principals;
               principals.kind = KindPrincipalCollection;
               principals.href = AnsiString(CardDavServer::ContextPath) + "principals/";
               xml += ResponseFor(context, principals, nullptr, request);

               Resource homes;
               homes.kind = KindHomeCollection;
               homes.href = AnsiString(CardDavServer::ContextPath) + "addressbooks/";
               xml += ResponseFor(context, homes, nullptr, request);

               Resource calendars;
               calendars.kind = KindCalendarsCollection;
               calendars.href = CalDavServer::CalendarsPath;
               xml += ResponseFor(context, calendars, nullptr, request);
               break;
            }
            case KindPrincipalCollection:
            {
               Resource principal;
               principal.kind = KindPrincipal;
               principal.href = context.principalHref;
               xml += ResponseFor(context, principal, nullptr, request);
               break;
            }
            case KindHomeCollection:
            {
               Resource home;
               home.kind = KindHome;
               home.href = context.homeHref;
               xml += ResponseFor(context, home, nullptr, request);
               break;
            }
            case KindHome:
            {
               // Every book the caller can see: its own, then the ones shared
               // with it. This is how a phone finds a shared address book at all.
               if (!EnsureBooks(context))
                  return Text(500, "the address books could not be read");
               for (size_t i = 0; i < context.books.size(); i++)
               {
                  Resource book;
                  book.kind = KindAddressBook;
                  book.book = context.books[i];
                  book.href = context.books[i].href;
                  xml += ResponseFor(context, book, nullptr, request);
               }

               // The organisation's directory beside the books, so a client
               // that walks the home finds it without being told its address.
               Resource directory;
               directory.kind = KindDirectory;
               directory.href = context.directoryHref;
               xml += ResponseFor(context, directory, nullptr, request);
               break;
            }
            case KindAddressBook:
            {
               const std::vector<Member> *members = MembersOf(context, resource.book);
               if (!members)
                  return Text(500, "the contacts could not be read");
               for (size_t i = 0; i < members->size(); i++)
                  xml += ResponseForMember(context, resource.book, (*members)[i], request);
               break;
            }
            case KindDirectory:
            {
               const std::vector<Member> *members = DirectoryMembers(context);
               if (!members)
                  return Text(500, "the directory could not be read");
               for (size_t i = 0; i < members->size(); i++)
                  xml += ResponseForMember(context, resource.book, (*members)[i], request, true);
               break;
            }
            default:
               break;
            }
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // Nothing here is writable by PROPPATCH: the one address book has the
      // name it has. Each property asked for is answered 403 in the 207, which
      // is how RFC 4918 section 9.2 says to refuse some and not all - and a
      // client that tries to name its address book learns so per property
      // rather than from a bare failure.
      HttpResponse HandleProppatch(Context &context, const Resource &resource)
      {
         if (resource.kind == KindContactSlot)
            return NotFound();

         // The directory is not this account's to name or to annotate. The
         // same 403 with the same reason as a PUT and a DELETE into it: the
         // three ways of writing to a read-only collection are refused alike,
         // and the doc and the row say so.
         if (IsDirectoryKind(resource.kind))
            return Text(403, "the directory is the organisation's and is served here read-only; its properties are the "
                             "server's and cannot be set or removed by a client");

         XmlElement root;
         AnsiString problem;
         if (!ReadBody(context.request, root, problem))
            return Text(400, "the PROPPATCH body is not well-formed XML: " + problem);
         if (!root.Is(NsDav, "propertyupdate"))
            return Text(400, "the PROPPATCH body must be a DAV:propertyupdate element");

         AnsiString refused;
         for (size_t i = 0; i < root.children.size(); i++)
         {
            const XmlElement &action = root.children[i];
            if (!action.Is(NsDav, "set") && !action.Is(NsDav, "remove"))
               continue;
            const XmlElement *prop = action.Child(NsDav, "prop");
            if (!prop)
               continue;
            for (size_t j = 0; j < prop->children.size(); j++)
            {
               PropertyName name;
               name.ns = prop->children[j].ns;
               name.name = prop->children[j].name;
               refused += PropertyElement(name, "");
            }
         }

         AnsiString xml = MultistatusOpen();
         xml += " <D:response>\r\n  " + Href(resource.href) + "\r\n";
         xml += "  <D:propstat><D:prop>" + refused + "</D:prop><D:status>HTTP/1.1 403 Forbidden</D:status>"
                "<D:responsedescription>The properties of this address book are fixed; none can be set or removed.</D:responsedescription></D:propstat>\r\n";
         xml += " </D:response>\r\n</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      HttpResponse HandleGet(Context &context, const Resource &resource, bool head)
      {
         if (resource.kind == KindRoot)
         {
            AnsiString text = "hMailServer CardDAV and CalDAV. Your principal is " + context.principalHref +
               ", your address books are under " + context.homeHref + " (contacts/ is your own, shared-<id>/ is one shared with you),"
               " your organisation's directory is " + context.directoryHref +
               " and your calendar is " + CalDavServer::HomeHref(context.addressUtf8) + "calendar/.";
            return Respond(200, "text/plain; charset=utf-8", head ? AnsiString("") : text + "\r\n");
         }

         if (resource.kind == KindContactSlot || resource.kind == KindDirectorySlot)
            return NotFound();

         if (resource.kind != KindContact && resource.kind != KindDirectoryCard)
            return MethodNotAllowed(resource);

         Member member;
         if (resource.kind == KindDirectoryCard)
         {
            const std::vector<Member> *members = DirectoryMembers(context);
            if (!members)
               return Text(500, "the directory could not be read");
            if (resource.directoryIndex < 0 || (size_t) resource.directoryIndex >= members->size())
               return NotFound();
            member = (*members)[resource.directoryIndex];
         }
         else
            FillMember(resource.book, resource.contact, member);

         AnsiString headers = "ETag: " + member.etag + "\r\n";

         AnsiString ifNoneMatch = Trimmed(context.request.Header("if-none-match"));
         if (!ifNoneMatch.IsEmpty() && ifNoneMatch != "*")
         {
            std::vector<AnsiString> tags = StringParser::SplitString(ifNoneMatch, ",");
            for (size_t i = 0; i < tags.size(); i++)
            {
               AnsiString tag = Trimmed(tags[i]);
               if (tag.StartsWith("W/"))
                  tag = tag.Mid(2);
               if (tag == member.etag)
                  return Respond(304, "text/vcard; charset=utf-8", "", headers);
            }
         }

         // CardOf: a directory card's photo is read here, for this one card
         // and no other.
         const AnsiString &card = CardOf(member);
         return Respond(200, "text/vcard; charset=utf-8", head ? AnsiString("") : card, headers);
      }

      bool IsVCardContentType(const AnsiString &contentType)
      {
         AnsiString type = Lower(Trimmed(contentType));
         int semicolon = type.Find(";");
         if (semicolon >= 0)
            type = Trimmed(type.Mid(0, semicolon));
         return type.IsEmpty() || type == "text/vcard" || type == "text/x-vcard" || type == "text/directory";
      }

      HttpResponse HandlePut(Context &context, const Resource &resource)
      {
         if (IsDirectoryKind(resource.kind))
            return Text(403, "the directory is the organisation's and is served here read-only; a person changes their own "
                             "entry in the webmail's Settings, and an administrator changes any of them");

         if (resource.kind != KindContact && resource.kind != KindContactSlot)
            return MethodNotAllowed(resource);

         // A book shared read-only. current-user-privilege-set said so before
         // the client tried, and this is the answer if it tries anyway.
         if (!resource.book.writable)
            return Text(403, "this address book is shared with you read-only");

         const HttpRequest &request = context.request;

         if (!IsVCardContentType(request.Header("content-type")))
            return DavError(415, "<C:supported-address-data/>", "The body must be a vCard: text/vcard.");

         if (request.body.GetLength() > MaxCardBytes)
            return DavError(403, "<C:max-resource-size/>", "The card is larger than " + IntText(MaxCardBytes) + " bytes.");

         std::vector<VCardProperty> properties;
         AnsiString problem;
         if (!VCard::Parse(request.body, properties, problem))
            return DavError(403, "<C:valid-address-data/>", "The body is not one vCard: " + problem + ".");

         AnsiString legacy;
         if (VCard::UsesLegacyEncoding(properties, legacy))
            return DavError(403, "<C:valid-address-data/>",
               "The card uses a vCard 2.1 encoding this server does not decode (" + legacy +
               "); send vCard 3.0 or 4.0, UTF-8 throughout.");

         // A group card (ContactGroups.h) has no EMAIL of its own: its name
         // is its FN, and the row's address is the marker made of its UID,
         // so that two groups with one UID are the same conflict two contacts
         // with one address are. Its members are kept as the client wrote
         // them and resolved when they are read.
         const bool isGroup = VCard::IsGroup(properties);

         // A group is never written into a book somebody else owns. The row
         // would be the OWNER'S - every row of a shared book is - and a group's
         // members resolve against its owner's whole address book, so a
         // writable share would otherwise be a way of asking for the contacts
         // of the books that were NOT shared: put a group here naming rows by
         // address or by the UID a row of theirs is served under, then read the
         // resolved members back. It would also plant a name the owner's own
         // compose expands. No group is shared, in either direction: what a
         // read-only collection already refuses, a writable one refuses here.
         if (isGroup && resource.book.shared)
            return DavError(403, "<C:valid-address-data/>",
               "A group is the address book owner's own entry and is not written into a book that is shared with you; "
               "make it in your own address book, where its members are yours.");

         // The same ceiling the API writes under, because the cost of reading
         // a group is the cost of its members and both surfaces write into one
         // address book: a card of a megabyte could otherwise name fifteen
         // thousand of them and be resolved on every listing of the book.
         if (isGroup)
         {
            std::vector<AnsiString> members;
            VCard::MemberReferences(properties, members);
            if ((int) members.size() > ContactGroups::MaximumMembers)
               return DavError(403, "<C:valid-address-data/>",
                  "A group holds at most " + IntText(ContactGroups::MaximumMembers) + " members here, and this card names " +
                  IntText((int) members.size()) + ".");
         }

         AnsiString nameUtf8;
         AnsiString addressUtf8;
         const bool hasAddress = VCard::ExtractNameAndAddress(properties, nameUtf8, addressUtf8);
         if (!isGroup && !hasAddress)
            return DavError(403, "<C:valid-address-data/>",
               "This address book keeps a name and one e-mail address per contact, and the card has no EMAIL; "
               "there is nothing to store it as. Add an e-mail address to the contact.");

         String name = Wide(nameUtf8);
         name.TrimLeft();
         name.TrimRight();
         if (name.GetLength() > ContactStore::MaximumNameLength)
            name = name.Mid(0, ContactStore::MaximumNameLength);
         if (isGroup && name.IsEmpty())
            return DavError(403, "<C:valid-address-data/>", "A group needs a name: the card has no FN.");

         // Every row written here is the BOOK OWNER'S, whoever is writing it:
         // a contact added to a book somebody shares with you is theirs, and
         // goes with the book if the share is taken back.
         const __int64 accountId = resource.book.ownerAccountId;
         const __int64 bookId = resource.book.record.id;

         // The card's own UID is kept; a card without one gets a stable one -
         // for an existing contact the one it had, for a new one derived from
         // the name it was created under - so the same card always carries the
         // same UID and a client never sees a contact change identity.
         AnsiString uid = VCard::UidOf(properties);
         if (uid.IsEmpty())
         {
            uid = resource.kind == KindContact
               ? (resource.contact.uid.IsEmpty() ? StableUid_(resource.book.ownerUtf8, resource.contact.id) : Utf8(resource.contact.uid))
               : StableUidFromSeed_("hMailServer.carddav.resource:" + resource.book.ownerUtf8 + ":" + resource.book.segment + ":" + resource.slotName);
         }
         if (isGroup && uid.GetLength() > ContactGroups::MaximumUidLength)
            return DavError(403, "<C:valid-address-data/>", "A group's UID is at most " + IntText(ContactGroups::MaximumUidLength) + " characters here.");

         String address = isGroup ? ContactStore::GroupAddress(Wide(uid)) : Wide(addressUtf8);
         if (!isGroup && !ContactStore::IsValidAddress(address))
            return DavError(403, "<C:valid-address-data/>", "The EMAIL is not one e-mail address: " + addressUtf8);

         // What a 409 says the other row has: a contact's address, a group's UID.
         const AnsiString identity = isGroup ? "the UID " + uid : "the address " + addressUtf8;

         // The card is kept as sent: what a GET returns is what the client
         // wrote, so the ETag answered here is the ETag of the stored
         // representation, as RFC 9110 section 9.3.4 requires of a PUT.
         const AnsiString &card = request.body;

         if (resource.kind == KindContact)
         {
            Member current;
            FillMember(resource.book, resource.contact, current);
            if (!PreconditionsHold(request, current.etag))
               return Text(412, "precondition failed: the contact has changed since it was read, or If-None-Match: * named an existing resource",
                  "ETag: " + current.etag + "\r\n");

            // The address is the row's identity in this store, and it is unique
            // per ACCOUNT rather than per book: moving this contact onto another
            // contact's address would be two rows for one address, which the
            // table refuses, and the other row may be in another book.
            __int64 other = 0;
            if (ContactStore::FindByAddress(accountId, address, other) && other != resource.contact.id)
            {
               ContactRecord otherRecord;
               AnsiString otherHref = ContactStore::Get(accountId, other, otherRecord) ? HrefOfVisibleContact(context, otherRecord) : AnsiString("");
               return Text(409, "another contact of this address book's account already has " + identity +
                  At(otherHref) + "; a contact is one address here, in one of its books, and a group one UID");
            }

            if (!ContactStore::UpdateCard(accountId, resource.contact.id, name, address, Wide(uid), Wide(card)))
               return Text(500, "the contact could not be saved");

            ContactRecord updated = resource.contact;
            updated.name = name;
            updated.address = address;
            updated.uid = Wide(uid);
            updated.vcard = Wide(card);
            Member written;
            FillMember(resource.book, updated, written);
            LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " updated contact " + StringParser::IntToString(updated.id) + ".");
            return Respond(204, "text/plain", "", "ETag: " + written.etag + "\r\n");
         }

         // A new resource, under the client's name. Nothing is there, so an
         // If-Match cannot hold; If-None-Match: * does.
         if (!PreconditionsHold(request, ""))
            return Text(412, "precondition failed: nothing exists at this URL for If-Match to match");

         // A contact is one address here. A card for an address the book
         // already holds - a contact the webmail collected, or one a client
         // created before under another name - is not a second contact and is
         // not this one either: the client asked to create, and rewriting the
         // other contact under its name would leave the client with two copies
         // of one row. 409 names the contact that has the address.
         __int64 existing = 0;
         ContactRecord record;
         if (ContactStore::FindByAddress(accountId, address, existing))
         {
            // A collected row is adopted only when it is ALREADY in this book.
            // The lookup above is per ACCOUNT, because the unique index is, so
            // the row it found may be in another of the owner's books - their
            // private default book is where every address they write to is
            // collected (ContactCard.cpp) - and adopting it there would take a
            // row out of a book they never shared and put it, rewritten under
            // the writer's card, into one they did. A book shared with somebody
            // is not a way to move the owner's contacts into it.
            ContactRecord existingRecord;
            if (!isGroup && ContactStore::Get(accountId, existing, existingRecord) &&
                existingRecord.source == ContactStore::SourceCollected && InBook(resource.book, existingRecord))
            {
               // A collected address the phone was never shown: the phone's card
               // becomes the contact, under the phone's name, kept from now on.
               if (!ContactStore::UpdateCard(accountId, existing, name, address, Wide(uid), Wide(card)) ||
                   !ContactStore::SetSource(accountId, existing, ContactStore::SourceManual) ||
                   !ContactStore::Get(accountId, existing, record))
                  return Text(500, "the contact could not be saved");

               // The client's own name for it, so its next listing finds it there.
               if (record.uri.IsEmpty())
               {
                  SQLCommand rename("update hm_contacts set contacturi = @URI where contactid = @ID");
                  rename.AddParameter("@URI", Wide(resource.slotName));
                  rename.AddParameter("@ID", existing);
                  Application::Instance()->GetDBManager()->Execute(rename);
                  record.uri = Wide(resource.slotName);
               }

               Member kept;
               FillMember(resource.book, record, kept);
               LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " kept collected contact " + StringParser::IntToString(record.id) + " at " + String(kept.href) + ".");

               return Respond(201, "text/plain", "", "ETag: " + kept.etag + "\r\nLocation: " + kept.href + "\r\n");
            }

            AnsiString existingHref = ContactStore::Get(accountId, existing, existingRecord) ? HrefOfVisibleContact(context, existingRecord) : AnsiString("");
            return Text(409, "this address book's account already has a contact with " + identity +
               At(existingHref) + "; a contact is one address here, in one of its books, and a group one UID - update that one, or delete it first");
         }

         // And the ceiling on how many groups one book holds, for the same
         // reason: every read of the book resolves them all.
         if (isGroup)
         {
            int groups = 0;
            if (ContactStore::CountGroups(accountId, groups) && groups >= ContactGroups::MaximumGroups)
               return Text(507, "this address book already holds the " + IntText(ContactGroups::MaximumGroups) +
                  " groups it may; delete one before making another");
         }

         if (!ContactStore::InsertCard(accountId, bookId, name, address, Wide(resource.slotName), Wide(uid), Wide(card), record))
         {
            // Two clients creating the same address at once: the second insert
            // is refused by the table's unique index, and the answer is the same
            // 409 as if the first had come a moment earlier.
            if (ContactStore::FindByAddress(accountId, address, existing))
            {
               ContactRecord existingRecord;
               AnsiString existingHref = ContactStore::Get(accountId, existing, existingRecord) ? HrefOfVisibleContact(context, existingRecord) : AnsiString("");
               return Text(409, "this address book's account already has a contact with " + identity + At(existingHref));
            }
            return Text(500, "the contact could not be saved");
         }

         Member created;
         FillMember(resource.book, record, created);
         LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " created contact " + StringParser::IntToString(record.id) + " at " + String(created.href) + ".");

         // The contact lives where the client put it: the Location is the
         // request's own URL, canonically encoded.
         return Respond(201, "text/plain", "", "ETag: " + created.etag + "\r\nLocation: " + created.href + "\r\n");
      }

      HttpResponse HandleDelete(Context &context, const Resource &resource)
      {
         if (IsDirectoryKind(resource.kind))
            return Text(403, "the directory is the organisation's and is served here read-only; a person leaves it with the "
                             "opt-out on the webmail's Settings page");

         if (resource.kind == KindContactSlot)
            return NotFound();

         if (resource.kind == KindAddressBook)
         {
            // A collection somebody shares with the caller: the share is given
            // back and the book stays where it is, exactly as leaving a shared
            // calendar does. The caller's own books are made and removed from
            // the webmail, not from here.
            if (!resource.book.shared)
               return Text(403, "your own address books are not deleted over CardDAV; use the Contacts page");

            // Only the share made to this account. One made to the whole domain
            // is not this person's to take back - doing so here would take the
            // book away from everybody else in the domain as well.
            AddressBookShareRecord own;
            if (!AddressBookStore::ShareOf(resource.book.record.id, context.account->GetID(), 0, own))
               return Text(403, "this address book is shared with your whole domain; only its owner can take that share back");

            if (!AddressBookStore::Unshare(resource.book.record.id, context.account->GetID(), 0))
               return Text(500, "the share could not be removed");

            LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " left the shared address book " +
               String(Int64Text(resource.book.record.id)) + ".");
            return Respond(204, "text/plain", "");
         }

         if (resource.kind != KindContact)
            return Text(403, "the collections above an address book cannot be deleted");

         if (!resource.book.writable)
            return Text(403, "this address book is shared with you read-only");

         Member current;
         FillMember(resource.book, resource.contact, current);
         if (!PreconditionsHold(context.request, current.etag))
            return Text(412, "precondition failed: the contact has changed since it was read", "ETag: " + current.etag + "\r\n");

         // The row is the book owner's, whoever is deleting it.
         if (!ContactStore::Delete(resource.book.ownerAccountId, resource.contact.id))
            return NotFound();

         // The groups that named it no longer do (ContactGroups.h) - the OWNER'S
         // groups, since the row was theirs.
         std::shared_ptr<const Account> owner = resource.book.shared
            ? CacheContainer::Instance()->GetAccount(resource.book.ownerAccountId)
            : context.account;
         if (owner)
            ContactGroups::ForgetMember(owner, resource.contact);

         LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " deleted contact " + StringParser::IntToString(resource.contact.id) + ".");
         return Respond(204, "text/plain", "");
      }

      //------------------------------------------------------------------------
      // The reports
      //------------------------------------------------------------------------

      HttpResponse HandleMultiget(Context &context, const Resource &resource, const XmlElement &report)
      {
         PropertyRequest request;
         if (const XmlElement *prop = report.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "getetag"));
            request.names.push_back(PropertyName(NsCardDav, "address-data"));
         }

         const bool directory = IsDirectoryKind(resource.kind);
         const std::vector<Member> *members = MembersFor(context, resource);
         if (!members)
            return Text(500, directory ? "the directory could not be read" : "the contacts could not be read");

         AnsiString xml = MultistatusOpen();
         size_t hrefs = 0;
         // An href asked for twice is answered once: five thousand copies of one
         // href, each answered with a megabyte card escaped for XML, was a
         // twenty-gigabyte response (the review of 14 September 2026). And the
         // whole multistatus is bounded, since even distinct cards add up.
         std::set<AnsiString> answered;
         const size_t MaxMultistatusBytes = 64 * 1024 * 1024;
         for (size_t i = 0; i < report.children.size(); i++)
         {
            if (!report.children[i].Is(NsDav, "href"))
               continue;
            if (++hrefs > MaxMultigetHrefs)
               return Text(400, "too many hrefs in one multiget");
            if (xml.GetLength() > MaxMultistatusBytes)
               return Text(507, "the response would be too large; ask for fewer cards in one multiget");

            // An href may be absolute; only its path is compared, decoded, so
            // that a client's own encoding of the address still matches.
            AnsiString href = Trimmed(report.children[i].text);
            int scheme = href.Find("://");
            if (scheme >= 0)
            {
               int slash = href.Find("/", scheme + 3);
               href = slash >= 0 ? href.Mid(slash) : AnsiString("/");
            }

            // Matched against the members already in memory, decoded on both
            // sides so a client's own encoding of the address still matches;
            // no lookup in the store per href.
            AnsiString wanted = PercentDecode(href);
            if (!answered.insert(wanted).second)
               continue;
            const Member *found = nullptr;
            for (size_t j = 0; !found && j < members->size(); j++)
            {
               if (PercentDecode((*members)[j].href) == wanted)
                  found = &(*members)[j];
            }

            if (found)
               xml += ResponseForMember(context, resource.book, *found, request, directory);
            else
               xml += " <D:response>\r\n  " + Href(href) + "\r\n  <D:status>HTTP/1.1 404 Not Found</D:status>\r\n </D:response>\r\n";
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      struct TextMatch
      {
         TextMatch() : negate(false) { }

         AnsiString text;        // UTF-8
         AnsiString collation;   // i;unicode-casemap by default
         AnsiString matchType;   // contains by default
         bool negate;
      };

      struct PropFilter
      {
         PropFilter() : isNotDefined(false), allOf(false) { }

         AnsiString name;        // upper-cased vCard property name
         bool isNotDefined;
         bool allOf;             // test="allof"; anyof by default
         std::vector<TextMatch> textMatches;
      };

      struct Filter
      {
         Filter() : allOf(false) { }

         bool allOf;
         std::vector<PropFilter> propFilters;
      };

      void ReadFilter(const XmlElement &filter, Filter &result)
      {
         result.allOf = Lower(filter.Attribute("test")) == "allof";

         for (size_t i = 0; i < filter.children.size(); i++)
         {
            const XmlElement &element = filter.children[i];
            if (!element.Is(NsCardDav, "prop-filter"))
               continue;

            PropFilter propFilter;
            propFilter.name = element.Attribute("name");
            propFilter.name.ToUpper();
            propFilter.allOf = Lower(element.Attribute("test")) == "allof";

            for (size_t j = 0; j < element.children.size(); j++)
            {
               const XmlElement &condition = element.children[j];
               if (condition.Is(NsCardDav, "is-not-defined"))
                  propFilter.isNotDefined = true;
               else if (condition.Is(NsCardDav, "text-match"))
               {
                  TextMatch match;
                  match.text = condition.text;
                  match.collation = Lower(condition.Attribute("collation"));
                  match.matchType = Lower(condition.Attribute("match-type"));
                  match.negate = Lower(condition.Attribute("negate-condition")) == "yes";
                  propFilter.textMatches.push_back(match);
               }
               // param-filter is not evaluated: the cards here carry no
               // parameter a client filters on, and a filter that cannot be
               // applied does not narrow the answer.
            }

            result.propFilters.push_back(propFilter);
         }
      }

      // The values a contact has for a vCard property, as the card would
      // carry them.
      void PropertyValues(const Member &member, const AnsiString &name, std::vector<AnsiString> &values)
      {
         AnsiString nameUtf8 = Utf8(member.record.name);
         AnsiString addressUtf8 = Utf8(member.record.address);

         // A group has a name and a UID and no EMAIL - its marker is not one -
         // and answers KIND in either spelling, so a client that asks the book
         // for its groups gets them and one that asks for an address does not.
         const bool group = ContactGroups::IsGroup(member.record);

         if (name == "FN")
            values.push_back(nameUtf8.IsEmpty() && !group ? addressUtf8 : nameUtf8);
         else if (name == "N")
         {
            AnsiString family;
            AnsiString given;
            if (group)
               family = nameUtf8;
            else
               VCard::SplitName(nameUtf8, family, given);
            if (!family.IsEmpty())
               values.push_back(family);
            if (!given.IsEmpty())
               values.push_back(given);
            if (values.empty())
               values.push_back("");
         }
         else if (name == "EMAIL")
         {
            if (!group)
               values.push_back(addressUtf8);
         }
         else if (name == "UID")
            values.push_back(member.uid);
         else if (name == "KIND" || name == "X-ADDRESSBOOKSERVER-KIND")
         {
            if (group)
               values.push_back("group");
         }
      }

      bool TextMatches(const TextMatch &match, const AnsiString &value)
      {
         bool caseSensitive = match.collation == "i;octet";
         AnsiString haystack = caseSensitive ? value : Lower(value);
         AnsiString needle = caseSensitive ? match.text : Lower(match.text);

         bool result;
         if (match.matchType == "equals")
            result = haystack == needle;
         else if (match.matchType == "starts-with")
            result = haystack.StartsWith(needle);
         else if (match.matchType == "ends-with")
            result = haystack.EndsWith(needle);
         else
            result = haystack.Find(needle) >= 0;

         return match.negate ? !result : result;
      }

      bool PropFilterMatches(const PropFilter &filter, const Member &member)
      {
         std::vector<AnsiString> values;
         PropertyValues(member, filter.name, values);

         if (filter.isNotDefined)
            return values.empty();

         if (filter.textMatches.empty())
            return !values.empty();

         if (values.empty())
            return false;

         bool anyMatched = false;
         bool allMatched = true;
         for (size_t i = 0; i < filter.textMatches.size(); i++)
         {
            bool matched = false;
            for (size_t j = 0; !matched && j < values.size(); j++)
               matched = TextMatches(filter.textMatches[i], values[j]);
            anyMatched = anyMatched || matched;
            allMatched = allMatched && matched;
         }

         return filter.allOf ? allMatched : anyMatched;
      }

      bool FilterMatches(const Filter &filter, const Member &member)
      {
         if (filter.propFilters.empty())
            return true;

         bool anyMatched = false;
         bool allMatched = true;
         for (size_t i = 0; i < filter.propFilters.size(); i++)
         {
            bool matched = PropFilterMatches(filter.propFilters[i], member);
            anyMatched = anyMatched || matched;
            allMatched = allMatched && matched;
         }

         return filter.allOf ? allMatched : anyMatched;
      }

      HttpResponse HandleQuery(Context &context, const Resource &resource, const XmlElement &report)
      {
         PropertyRequest request;
         if (const XmlElement *prop = report.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "getetag"));
            request.names.push_back(PropertyName(NsCardDav, "address-data"));
         }

         Filter filter;
         if (const XmlElement *filterElement = report.Child(NsCardDav, "filter"))
            ReadFilter(*filterElement, filter);

         int limit = -1;
         if (const XmlElement *limitElement = report.Child(NsCardDav, "limit"))
         {
            if (const XmlElement *results = limitElement->Child(NsCardDav, "nresults"))
            {
               limit = atoi(results->text.c_str());
               if (limit < 0)
                  limit = 0;
            }
         }

         const bool directory = IsDirectoryKind(resource.kind);
         const std::vector<Member> *members = MembersFor(context, resource);
         if (!members)
            return Text(500, directory ? "the directory could not be read" : "the contacts could not be read");

         AnsiString xml = MultistatusOpen();
         int answered = 0;
         for (size_t i = 0; i < members->size(); i++)
         {
            if (limit >= 0 && answered >= limit)
               break;
            if (!FilterMatches(filter, (*members)[i]))
               continue;
            xml += ResponseForMember(context, resource.book, (*members)[i], request, directory);
            answered++;
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      HttpResponse HandleSyncCollection(Context &context, const Resource &resource, const XmlElement &report)
      {
         PropertyRequest request;
         if (const XmlElement *prop = report.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "getetag"));
         }

         AnsiString presented;
         if (const XmlElement *token = report.Child(NsDav, "sync-token"))
            presented = Trimmed(token->text);

         const bool directory = IsDirectoryKind(resource.kind);
         AnsiString tag;
         if (!CollectionTagFor(context, resource, tag))
            return Text(500, directory ? "the directory could not be read" : "the contacts could not be read");
         AnsiString current = SyncTokenFor(tag);

         const std::vector<Member> *members = MembersFor(context, resource);
         if (!members)
            return Text(500, directory ? "the directory could not be read" : "the contacts could not be read");

         AnsiString xml = MultistatusOpen();

         if (presented.IsEmpty())
         {
            // The initial sync: everything.
            for (size_t i = 0; i < members->size(); i++)
               xml += ResponseForMember(context, resource.book, (*members)[i], request, directory);
         }
         else if (presented != current)
         {
            // The store keeps no change log, so a token that is not the
            // present state cannot be turned into the changes since it. The
            // client is told so and lists again (RFC 6578 section 3.2).
            return DavError(403, "<D:valid-sync-token/>",
               "The sync token is not the address book's current state, and this server keeps no change log to answer "
               "what changed since; ask again without a token for the whole address book.");
         }

         xml += " <D:sync-token>" + XmlEscape(current) + "</D:sync-token>\r\n";
         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // The value a principal-property-search term is matched against, for
      // the properties a room finder searches by; false for any other.
      bool SearchedValue(const Resource &principal, const AnsiString &ns, const AnsiString &name, AnsiString &value)
      {
         if (ns == NsDav && name == "displayname")
            value = principal.principalName.IsEmpty() ? principal.principalAddressUtf8 : principal.principalName;
         else if (ns == NsCalDav && name == "calendar-user-address-set")
            value = "mailto:" + principal.principalAddressUtf8;
         else if (ns == NsCalDav && name == "calendar-user-type")
            value = CalendarUserTypeOf(principal.principalKind);
         else if (ns == NsCalendarServer && name == "email-address-set")
            value = principal.principalAddressUtf8;
         else
            return false;
         return true;
      }

      // RFC 3744 section 9.4, on /dav/principals/: the rooms and the pieces
      // of equipment of the caller's own domain whose properties contain the
      // texts asked for, case-insensitively - all of them (test="allof", the
      // default) or any (test="anyof") - each answered with the properties
      // the report's DAV:prop names. People are not searched here: the
      // directory (DirectoryStore.h) is where a colleague is found, and this
      // is where a client's room finder looks. Nothing is answered while the
      // directory is switched off.
      HttpResponse HandlePrincipalSearch(Context &context, const XmlElement &root)
      {
         struct Term
         {
            AnsiString ns;
            AnsiString name;
            AnsiString match;
         };

         std::vector<Term> terms;
         for (size_t i = 0; i < root.children.size(); i++)
         {
            const XmlElement &search = root.children[i];
            if (!search.Is(NsDav, "property-search"))
               continue;
            const XmlElement *prop = search.Child(NsDav, "prop");
            const XmlElement *match = search.Child(NsDav, "match");
            if (!prop || !match)
               return Text(400, "a DAV:property-search names a DAV:prop and a DAV:match");
            for (size_t p = 0; p < prop->children.size(); p++)
            {
               Term term;
               term.ns = prop->children[p].ns;
               term.name = prop->children[p].name;
               term.match = Lower(Trimmed(match->text));
               terms.push_back(term);
            }
         }
         const bool anyOf = Lower(root.Attribute("test")) == "anyof";

         PropertyRequest request;
         if (const XmlElement *prop = root.Child(NsDav, "prop"))
            ReadPropertyRequest(prop, request);
         else
         {
            request.mode = ModeProp;
            request.names.push_back(PropertyName(NsDav, "displayname"));
            request.names.push_back(PropertyName(NsCalDav, "calendar-user-address-set"));
            request.names.push_back(PropertyName(NsCalDav, "calendar-user-type"));
         }

         AnsiString xml = MultistatusOpen();

         if (DirectoryStore::Enabled())
         {
            std::vector<DirectoryEntry> entries;
            if (!DirectoryStore::List(context.domainId, entries))
               return Text(500, "the directory could not be read");

            const size_t MaxPrincipalResults = 500;
            size_t answered = 0;
            for (size_t i = 0; i < entries.size() && answered < MaxPrincipalResults; i++)
            {
               const DirectoryEntry &entry = entries[i];
               if (entry.list || (entry.kind != Account::KindRoom && entry.kind != Account::KindEquipment))
                  continue;

               Resource principal;
               FillResourcePrincipal(Utf8(entry.address), Utf8(entry.name), entry.kind, principal);

               bool all = true;
               bool some = false;
               for (size_t t = 0; t < terms.size(); t++)
               {
                  AnsiString value;
                  const bool matched = SearchedValue(principal, terms[t].ns, terms[t].name, value) &&
                                       Lower(value).Find(terms[t].match) >= 0;
                  all = all && matched;
                  some = some || matched;
               }
               if (!terms.empty() && !(anyOf ? some : all))
                  continue;

               xml += ResponseFor(context, principal, nullptr, request);
               answered++;
            }
         }

         xml += "</D:multistatus>\r\n";
         return Xml(207, xml);
      }

      // RFC 3744 section 9.5: what the search above searches by.
      HttpResponse HandlePrincipalSearchSet()
      {
         AnsiString xml = "<D:principal-search-property-set xmlns:D=\"DAV:\" xmlns:CAL=\"urn:ietf:params:xml:ns:caldav\">"
            "<D:principal-search-property><D:prop><D:displayname/></D:prop><D:description xml:lang=\"en\">Name</D:description></D:principal-search-property>"
            "<D:principal-search-property><D:prop><CAL:calendar-user-address-set/></D:prop><D:description xml:lang=\"en\">Address</D:description></D:principal-search-property>"
            "<D:principal-search-property><D:prop><CAL:calendar-user-type/></D:prop><D:description xml:lang=\"en\">Kind</D:description></D:principal-search-property>"
            "</D:principal-search-property-set>\r\n";
         return Xml(200, xml);
      }

      HttpResponse HandleReport(Context &context, const Resource &resource)
      {
         if (resource.kind == KindContactSlot || resource.kind == KindDirectorySlot)
            return NotFound();

         XmlElement root;
         AnsiString problem;
         if (!ReadBody(context.request, root, problem))
            return Text(400, "the REPORT body is not well-formed XML: " + problem);

         // The principals answer the two reports of RFC 3744 a room finder
         // uses, and nothing else.
         if (resource.kind == KindPrincipalCollection || resource.kind == KindPrincipal)
         {
            if (root.Is(NsDav, "principal-property-search"))
               return HandlePrincipalSearch(context, root);
            if (root.Is(NsDav, "principal-search-property-set"))
               return HandlePrincipalSearchSet();
            return DavError(403, "<D:supported-report/>", "The reports on the principals are principal-property-search and principal-search-property-set.");
         }

         bool multiget = root.Is(NsCardDav, "addressbook-multiget");
         bool query = root.Is(NsCardDav, "addressbook-query");
         bool sync = root.Is(NsDav, "sync-collection");

         if (!multiget && !query && !sync)
            return DavError(403, "<D:supported-report/>", "The reports here are addressbook-multiget, addressbook-query and sync-collection.");

         // multiget is allowed at an address book and at a contact; the other
         // two are questions about the collection. The directory answers all
         // three the same way, since a phone that subscribes to it polls it
         // exactly as it polls the account's own books.
         bool onBook = resource.kind == KindAddressBook || resource.kind == KindDirectory;
         bool onContact = resource.kind == KindContact || resource.kind == KindDirectoryCard;
         if (!onBook && !(multiget && onContact))
            return DavError(403, "<D:supported-report/>", "This report applies to an address book under " + context.homeHref +
               ", or to the directory, " + context.directoryHref + ".");

         if (multiget)
            return HandleMultiget(context, resource, root);
         if (query)
            return HandleQuery(context, resource, root);
         return HandleSyncCollection(context, resource, root);
      }
   }

   bool
   CardDavServer::IsDavTarget(const AnsiString &target)
   {
      AnsiString path = target;
      int query = path.Find("?");
      if (query >= 0)
         path = path.Mid(0, query);

      return path == "/dav" || path.StartsWith(ContextPath);
   }

   bool
   CardDavServer::IsLargeRequest(const AnsiString &method, const AnsiString &target)
   {
      if (!IsDavTarget(target))
         return false;

      return method == "PUT" || method == "REPORT";
   }

   AnsiString
   CardDavServer::StableContactUid(const AnsiString &accountAddressUtf8, __int64 contactId)
   {
      return StableUid_(accountAddressUtf8, contactId);
   }

   HttpResponse
   CardDavServer::Handle(const HttpRequest &request, bool over_https)
   {
      // Basic puts the account's password on the wire, so this surface is
      // HTTPS only - the same rule as the Apple configuration profile on this
      // listener, for the same reason. Refused before the credential is
      // looked at, so a password sent in clear is at least not also checked.
      if (!over_https)
      {
         return Text(403,
            "CardDAV is served over HTTPS only: HTTP Basic authentication would send the account's password in clear. "
            "Use the https:// address of this server (WebServicesHttpsPort), or put a TLS-terminating proxy in front "
            "of this listener that sets X-Forwarded-Proto: https.");
      }

      const AnsiString &method = request.method;

      Context context(request);
      bool disconnect = false;
      if (!Authenticate(request, "CardDAV", SignInStore::ProtocolDav, context.account, disconnect))
      {
         HttpResponse refusal = Unauthorized();
         // The connection closes with the refusal, whatever the lockout said:
         // IMAP delays a wrong password before answering (the tarpit), and this
         // listener has no delay, so a guess costs a new connection instead of
         // running at wire speed down one kept-alive socket.
         refusal.close = true;
         return refusal;
      }

      context.addressWide = context.account->GetAddress();
      context.addressWide.ToLower();
      context.addressUtf8 = Utf8(context.addressWide);
      AnsiString segment = PercentEncodeSegment(context.addressUtf8);
      context.principalHref = AnsiString(ContextPath) + "principals/" + segment + "/";
      context.homeHref = AnsiString(ContextPath) + "addressbooks/" + segment + "/";
      context.directoryHref = context.homeHref + "directory/";
      context.domainId = context.account->GetDomainID();

      // Only the four methods that answer with a card's content, or with a
      // property of one, need the directory listed to tell a card from a name
      // it does not serve; the rest are refused the same way either way.
      const bool readsCards = method == "GET" || method == "HEAD" || method == "PROPFIND" || method == "REPORT";

      Resource resource;
      if (!Resolve(context, request.target, resource, readsCards))
      {
         // A collection is named by the book it is, so a store that could not be
         // read makes every one of them unresolvable. Answering 404 for that
         // would tell a client its address book had been deleted, and a client
         // told that throws its copy away; 500 is a failure it retries.
         if (context.booksFailed)
            return Text(500, "the address books could not be read");

         return NotFound();
      }

      HttpResponse response;
      try
      {
         if (method == "OPTIONS")
            response = HandleOptions(resource);
         else if (method == "PROPFIND")
            response = HandlePropfind(context, resource);
         else if (method == "PROPPATCH")
            response = HandleProppatch(context, resource);
         else if (method == "REPORT")
            response = HandleReport(context, resource);
         else if (method == "GET")
            response = HandleGet(context, resource, false);
         else if (method == "HEAD")
            response = HandleGet(context, resource, true);
         else if (method == "PUT")
            response = HandlePut(context, resource);
         else if (method == "DELETE")
            response = HandleDelete(context, resource);
         else if (method == "MKCOL" || method == "MKCALENDAR")
            response = Text(403, "an address book is made from the Contacts page of the webmail or over /api/v1/me/address-books, not from here, and a calendar from the Calendar page; no collection can be made over DAV");
         else
            response = MethodNotAllowed(resource);
      }
      catch (...)
      {
         response = Text(500, "internal error");
      }

      response.close = response.close || disconnect;

      LOG_DEBUG("CardDAV: " + String(context.addressUtf8) + " " + String(method) + " " + String(request.target) + " -> " +
         StringParser::IntToString(response.status) + ".");

      return response;
   }
}
