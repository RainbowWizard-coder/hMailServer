// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Sending as an address elsewhere, proved with a code (Gmail's "Send mail as"):
// the reader adds an address they own at another provider, the server mails a
// six-digit code to it, and once the reader types the code back the address is
// one the account may write as - in the webmail's From picker and in MAIL FROM
// on an authenticated SMTP session alike (SMTPConnection::SenderPermittedFor).
//
// hm_senderaddresses (schema 6066) holds the addresses, lower-cased, with a name
// and whether they are proved; it is the account's own data and travels in the
// backup with it (AccountStores). The code is not stored: it lives in memory
// for an hour, five wrong tries end it, and a new one is sent no sooner than a
// minute after the last - so a restart asks for one to be sent again, and nothing
// in a database dump proves an address.
//
// An address of a domain on this server is refused here: the owner's alias or
// the post right on their inbox is how such an address is shared.
#pragma once

namespace HM
{
   class Account;

   class SenderAddresses
   {
   public:
      struct Entry
      {
         __int64 id = 0;
         String address;
         String name;
         bool verified = false;
         String created;
      };

      static bool List(__int64 accountId, std::vector<Entry> &entries);
      static bool Find(__int64 accountId, __int64 id, Entry &entry);
      static int Count(__int64 accountId);

      // Whether the account has proved this address. The address is compared
      // lower-cased.
      static bool IsVerified(__int64 accountId, const String &address);

      // A new, unproved address; id is set. False when the row could not be
      // written - most often because the account has it already.
      static bool Add(__int64 accountId, const String &address, const String &name, __int64 &id);
      static bool MarkVerified(__int64 accountId, __int64 id);
      static bool Delete(__int64 accountId, __int64 id);

      enum CodeOutcome
      {
         CodeRight,
         CodeWrong,
         CodeExpired,
         CodeLocked
      };

      // A new code for an address. Empty, with waitSeconds set, when the last one
      // went out less than a minute ago.
      static AnsiString NewCode(__int64 id, int &waitSeconds);

      // The code the reader typed. attemptsLeft is set for a wrong one.
      static CodeOutcome CheckCode(__int64 id, const AnsiString &code, int &attemptsLeft);

      // Mails the code to the address, from the postmaster of the account's
      // domain with the null sender. False when it could not be queued.
      static bool SendCode(std::shared_ptr<const Account> account, const Entry &entry, const AnsiString &code);

      // The DMARC policy the address's domain publishes for mail from it -
      // "reject", "quarantine", "none" - or "" when it publishes none. Mail this
      // server sends as the address is not aligned with that domain, so a
      // receiver that honours a quarantine or reject policy will not take it.
      static AnsiString PolicyOf(const String &address);
   };
}
