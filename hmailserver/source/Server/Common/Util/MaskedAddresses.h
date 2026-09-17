// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Masked addresses (Fastmail's Masked Email, Apple's Hide My Email): an address
// the reader makes in a click for one shop or one sign-up, delivering to their
// mailbox, sent from when they reply, and switched off the day it starts to
// receive spam - after which mail to it is refused at RCPT like mail to an
// address that never existed.
//
// hm_maskedaddresses (schema 6067) holds them: the address, unique across the
// server; the label the reader gave it; whether it is on; how many messages it
// has delivered. The server's aliases are the administrator's and are not used:
// a masked address is the reader's own, bounded per account by
// ReaderMaskedAddressesMax, and goes with the account when it is deleted.
//
// An address is made in the account's own domain: a short form of the label, a
// dot and six random characters - "shop.k3v9qa@example.com" - checked against the
// accounts, aliases, distribution lists and other masked addresses of the server.
// RecipientParser asks FindOwner after the account, alias and list lookups, so
// an administrator who later makes an account or an alias with the same address
// takes it over; SMTPConnection::SenderPermittedFor and the identity picker let
// the owner write as an address that is on.
#pragma once

namespace HM
{
   class Account;

   class MaskedAddresses
   {
   public:
      struct Entry
      {
         __int64 id = 0;
         __int64 account_id = 0;
         String address;
         String label;
         bool active = true;
         int received = 0;
         String created;
      };

      static bool List(__int64 accountId, std::vector<Entry> &entries);
      static bool Find(__int64 accountId, __int64 id, Entry &entry);
      static int Count(__int64 accountId);

      // The masked address an address is, whoever owns it. False when it is none.
      static bool FindByAddress(const String &address, Entry &entry);

      // Makes one for the account, in its domain, from the label. False with
      // problem when no free address could be found.
      static bool Create(std::shared_ptr<const Account> account, const String &label, Entry &entry, String &problem);

      static bool Update(__int64 accountId, __int64 id, const String &label, bool active);
      static bool Delete(__int64 accountId, __int64 id);

      // One more message delivered through it.
      static void CountDelivery(__int64 id);

      // The local part a label gives: lower-case letters and digits, at most 20,
      // "masked" when there are none. Public for MaskedAddressesTester.
      static String Slug(const String &label);
   };

   class MaskedAddressesTester
   {
   public:
      void Test();
   };
}
