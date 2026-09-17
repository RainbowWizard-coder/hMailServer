// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

namespace HM
{
   class Account;

   // What the page reads of a card a phone stored beside the name and the
   // address: the birthday, the note, the phones and whether there is a photo,
   // and the photo itself as an image; and the collecting of addresses into
   // the address book, which the webmail's send and an SMTP submission share.
   class ContactCard
   {
   public:

      struct Fields
      {
         Fields() : hasPhoto(false) { }

         String birthday;              // YYYY-MM-DD, or --MM-DD when the card has no year
         String note;
         std::vector<String> phones;
         bool hasPhoto;
      };

      // False when the text is not a card; the fields are then empty.
      static bool Read(const String &vcard, Fields &fields);

      // The embedded photo as bytes with its media type - a base64 PHOTO of
      // vCard 3 or a data: URI of vCard 4. False for no photo or one that is
      // only a URL.
      static bool Photo(const String &vcard, AnsiString &bytes, AnsiString &contentType);

      // Every entry - "Name <a@b>" or "a@b" - becomes a contact of the account
      // once, marked collected; the account's own address is left out, and
      // nothing here can fail the send that called it.
      static void Collect(std::shared_ptr<const Account> account, const std::vector<String> &entries);
   };
}
