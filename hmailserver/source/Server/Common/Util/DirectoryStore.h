// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The organisation's directory - Outlook's Global Address List, Google's
// directory - one domain at a time.
//
// What it answers is every active account and every enabled distribution list
// of ONE domain: the reader's own. There is no cross-domain form and no
// unauthenticated form, because a server that hosts twenty unrelated domains
// must not let one of them read another's names, and a listing of every
// address at a domain handed out without a password is an address list for
// whoever asks.
//
// The names and addresses come from hm_accounts and hm_distributionlists,
// which already hold them; hm_directory (schema 6078) is what an organisation
// adds on top - a title, a department, three telephone numbers, a photo, the
// administrator's note, and the opt-out the person sets. A row is made the
// first time a field is set, so an account with nothing set has no row and is
// listed from hm_accounts alone; the row goes when the account does
// (ON DELETE CASCADE).
//
// Two switches, both in hm_settings and neither in the ini file:
//
//    DirectoryEnabled        the directory is answered at all. On. A server
//                            where one domain holds people who are not one
//                            organisation should turn it off.
//    DirectoryUserEditable   a person may set their own title, department,
//                            telephone numbers and photo. On. With it off the
//                            administrator sets those and the person can still
//                            opt out, because the opt-out is the person's and
//                            nothing else - it is the one field this store
//                            never lets a switch take away.
//
// The opt-out hides the person from the listing, the completion, the directory
// page and the CardDAV collection. It does not hide the mailbox: mail still
// arrives, the address still works, and anybody who already knows it can still
// write. It is "leave me out of the list", not "make me invisible", and the
// doc says so.
#pragma once

#include <vector>

namespace HM
{
   class Account;

   // What hm_directory holds for one account. Every field is optional; a
   // record with none of them set is a record with no row.
   struct DirectoryRecord
   {
      __int64 accountId = 0;
      String title;
      String department;
      String phone;
      String mobile;
      String office;
      String note;          // the administrator's, never answered to a reader
      String photoType;     // image/jpeg, image/png or image/gif; empty with no photo
      String photo;         // the image bytes, base64, as the column keeps them
      bool optOut = false;
      String updated;       // hMailServer system date, or empty with no row
      bool stored = false;  // whether there is a row at all
   };

   // One line of the directory as a reader reads it.
   struct DirectoryEntry
   {
      __int64 id = 0;          // the account id, or the list id for a list
      bool list = false;
      String name;             // the person's name; empty when the account has none
      String address;
      bool hasPhoto = false;
      DirectoryRecord record;  // empty for a list
   };

   class DirectoryStore
   {
   public:
      // The two switches.
      static bool Enabled();
      // Whether this account's person may set their own entry: DirectoryUserEditable
      // is a setting with levels (docs/SettingLevels.md), the account's own, its
      // domain's, or the server's.
      static bool UserEditable(std::shared_ptr<const Account> account);

      // The columns' widths, which both surfaces hold to.
      static const int MaximumTitleLength = 100;
      static const int MaximumDepartmentLength = 100;
      static const int MaximumPhoneLength = 60;
      static const int MaximumOfficeLength = 100;
      static const int MaximumNoteLength = 255;

      // A photo is an image of at most this many bytes, decoded. 100 KB is
      // about eight times what a 200-pixel square costs as JPEG, and a listing
      // never carries one - the page fetches each from its own route.
      static const int MaximumPhotoBytes = 100 * 1024;

      // One account's row, or a record with stored false when there is none.
      // False when the table could not be read.
      static bool Get(__int64 accountId, DirectoryRecord &record);

      // Writes the row, making one when there is none. The photo is stored as
      // it is given - base64, with its media type - and cleared when both are
      // empty. An insert that loses the race to another first save of the same
      // account is tried once more as an update rather than answered as a
      // failure.
      static bool Save(const DirectoryRecord &record);

      // The photo of one account as the image it is. False when there is none.
      static bool Photo(__int64 accountId, AnsiString &bytes, AnsiString &contentType);

      // The photo of one account of this domain, and only when that account is
      // in the domain's directory: of this domain, active, not opted out. One
      // query, because a page draws one photo per person listed and deciding
      // this by listing the whole domain made a page of N people cost N
      // listings of N - an authenticated mailbox's way of making the server
      // work by the thousand. False when there is no such photo, which is the
      // same answer as "no such entry": the caller says no more than that.
      static bool ListedPhoto(__int64 domainId, __int64 accountId, AnsiString &bytes, AnsiString &contentType);

      // Whether these bytes are an image this store keeps, and which - by what
      // the bytes begin with and not by what the sender called them, because a
      // media type from a request is the sender's word. problem says why not.
      static bool ReadPhoto(const AnsiString &bytes, AnsiString &contentType, String &problem);

      // Every active, not-opted-out account and every enabled distribution list
      // of the domain, by name then address, with each account's row except its
      // photo. False when the tables could not be read.
      //
      // No photo, for anybody: two queries whatever the domain holds. A caller
      // that serves one photo reads that one (Photo, ListedPhoto); the CardDAV
      // collection fills a card's PHOTO only when a client asks for that card's
      // body, and its ETag is computed so that it does not have to.
      static bool List(__int64 domainId, std::vector<DirectoryEntry> &entries);

      // The display name of an account: "Given Family" from the two name
      // columns, trimmed; empty when the account has neither.
      static String NameOf(const String &firstName, const String &lastName);

      // A vCard 3.0 for one entry, which is what CardDAV serves and what the
      // page's "add to contacts" writes: UID, FN, N, EMAIL, and the TITLE, ORG,
      // TEL and PHOTO the entry has. The UID is stable for the address, so a
      // phone that syncs the directory sees one card per person rather than a
      // new one every time.
      static AnsiString GenerateCard(const AnsiString &uid, const DirectoryEntry &entry);

      // The UID a directory card is served under: derived from the domain's
      // name and the entry's address, so the same person gives the same UID.
      static AnsiString StableUid(const AnsiString &addressUtf8, bool list);

   private:
      // One insert or one update of the row, which Save chooses between.
      static bool Write_(const DirectoryRecord &record, bool update);
   };
}
