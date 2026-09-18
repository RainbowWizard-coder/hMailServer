// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "ContactCard.h"
#include "ContactStore.h"
#include "AddressBookStore.h"
#include "VCard.h"
#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../BO/Account.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
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

      // A BDAY as the page shows it: 1985-04-12 as written, 19850412 (vCard 4)
      // as 1985-04-12, --0412 (no year) as --04-12; a time part is dropped.
      AnsiString NormalizeDate(const AnsiString &raw)
      {
         AnsiString value = raw;
         size_t t = value.find('T');
         if (t != AnsiString::npos)
            value = value.substr(0, t);

         bool digits = !value.empty();
         for (size_t i = 0; digits && i < value.size(); i++)
            digits = value.c_str()[i] >= '0' && value.c_str()[i] <= '9';

         if (digits && value.size() == 8)
            return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);

         if (value.size() == 6 && value.compare(0, 2, "--") == 0)
            return "--" + value.substr(2, 2) + "-" + value.substr(4, 2);

         return value;
      }

      bool ParseCard(const String &vcard, std::vector<VCardProperty> &properties)
      {
         if (vcard.IsEmpty())
            return false;

         AnsiString problem;
         return VCard::Parse(Utf8(vcard), properties, problem);
      }
   }

   bool
   ContactCard::Read(const String &vcard, Fields &fields)
   {
      fields = Fields();

      std::vector<VCardProperty> properties;
      if (!ParseCard(vcard, properties))
         return false;

      for (const VCardProperty &property : properties)
      {
         if (property.name == "BDAY")
            fields.birthday = Wide(NormalizeDate(VCard::Unescape(property.value)));
         else if (property.name == "NOTE" && fields.note.IsEmpty())
            fields.note = Wide(VCard::Unescape(property.value));
         else if (property.name == "TEL")
         {
            String phone = Wide(VCard::Unescape(property.value));
            phone.Trim();
            if (!phone.IsEmpty())
               fields.phones.push_back(phone);
         }
         else if (property.name == "PHOTO" && !property.value.empty())
            fields.hasPhoto = true;
      }

      return true;
   }

   bool
   ContactCard::Photo(const String &vcard, AnsiString &bytes, AnsiString &contentType)
   {
      bytes = "";
      contentType = "";

      std::vector<VCardProperty> properties;
      if (!ParseCard(vcard, properties))
         return false;

      for (const VCardProperty &property : properties)
      {
         if (property.name != "PHOTO" || property.value.empty())
            continue;

         AnsiString payload = property.value;
         AnsiString type;

         if (payload.compare(0, 5, "data:") == 0)
         {
            // vCard 4: data:image/png;base64,....
            size_t comma = payload.find(',');
            if (comma == AnsiString::npos || payload.find(";base64", 0) == AnsiString::npos)
               return false;
            type = payload.substr(5, payload.find(';') - 5);
            payload = payload.substr(comma + 1);
         }
         else
         {
            // vCard 3: PHOTO;ENCODING=b;TYPE=JPEG:.... - anything else is a URL,
            // which is not the server's to fetch.
            AnsiString encoding = property.Parameter("ENCODING");
            std::transform(encoding.begin(), encoding.end(), encoding.begin(), ::toupper);
            if (encoding != "B" && encoding != "BASE64")
               return false;

            AnsiString kind = property.Parameter("TYPE");
            std::transform(kind.begin(), kind.end(), kind.begin(), ::tolower);
            if (kind.empty())
               kind = "jpeg";
            if (kind == "jpg")
               kind = "jpeg";
            type = kind.compare(0, 6, "image/") == 0 ? kind : "image/" + kind;
         }

         AnsiString clean;
         for (size_t i = 0; i < payload.size(); i++)
         {
            const char c = payload.c_str()[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
               clean += c;
         }

         if (clean.empty())
            return false;

         bytes = Base64::Decode(clean.c_str(), (int) clean.size());
         contentType = type;
         return !bytes.empty();
      }

      return false;
   }

   void
   ContactCard::Collect(std::shared_ptr<const Account> account, const std::vector<String> &entries)
   {
      if (!account)
         return;

      String own = account->GetAddress();
      own.ToLower();

      // A collected address goes into the account's default book: it is a
      // suggestion the person has not kept, and putting one into a book they
      // share with a team would put every address they write to in front of it.
      const __int64 bookId = AddressBookStore::DefaultBookId(account->GetID());

      for (const String &entry : entries)
      {
         String name;
         String address;
         ContactStore::SplitEntry(entry, name, address);
         if (!ContactStore::IsValidAddress(address) || address == own)
            continue;

         __int64 existing = 0;
         if (ContactStore::FindByAddress(account->GetID(), address, existing))
            continue;

         ContactRecord inserted;
         ContactStore::Insert(account->GetID(), bookId, name, address, ContactStore::SourceCollected, inserted);
      }
   }
}
