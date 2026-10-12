// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <ctime>

namespace HM
{
   // BATV (Bounce Address Tag Validation), "prvs" Simple Private Signature
   // scheme (draft-levine-smtp-batv). The server signs the envelope MAIL FROM of
   // its own outbound mail so that any bounce returned to that address can be
   // validated; forged bounces (backscatter) addressed to an unsigned or tampered
   // tag are rejected.
   //
   //    prvs=<K><DDD><SSSSSS>=local-part@domain
   //
   // K   = single-digit key number (fixed '1').
   // DDD = 3-digit day number (days since the epoch, modulo 1000) used for expiry.
   // S   = first 6 hex digits of an HMAC-SHA256 over the signed fields.
   class BATV
   {
   public:
      // Signs mailbox into a prvs-tagged address at the same domain. Returns an
      // empty string if mailbox is empty/invalid, already tagged, or no secret is
      // configured.
      static String Sign(const String &mailbox, const String &secret);

      // Returns true if the local-part of address is a prvs-tagged address.
      static bool IsPrvs(const String &address);

      // Validates a prvs-tagged address and returns the original mailbox. Returns
      // true only when the HMAC signature is valid and the day number is in range.
      static bool Verify(const String &address, const String &secret, String &originalMailbox);

      // The two above at the time given rather than now: what they do, and what the
      // in-process tests hold the day window to.
      static String SignAt(const String &mailbox, const String &secret, time_t now);
      static bool VerifyAt(const String &address, const String &secret, time_t now, String &originalMailbox);

   private:
      static int DayNumberAt_(time_t when);
      static bool DayNumberInRange_(int dayNumber, time_t now);
      static String ComputeHash_(const String &secret, const String &keyAndDay, const String &mailbox);
   };
}
