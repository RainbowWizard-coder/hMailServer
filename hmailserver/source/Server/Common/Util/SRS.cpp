// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "SRS.h"

#include "Hashing/HashCreator.h"
#include "Parsing/StringParser.h"
#include "Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // RFC 4648 base32 alphabet (without padding). 1024 = 32^2, so a day-slot
   // taken modulo 1024 always encodes to exactly two characters.
   static const wchar_t *kBase32Alphabet = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

   // How many days an SRS address remains valid for.
   static const int kMaxAgeDays = 21;

   // The tolerance for a clock - another node's sharing the secret - a little ahead
   // of this one's, which stamps tomorrow's day in the last moments of today:
   // tomorrow's stamp is taken in the last hour of today, and at no other time,
   // where any address that read one day ahead was taken - and modulo the cycle of
   // 1024 days one day ahead is also 1023 days old, so an address that old was let
   // through as fresh (upstream #649).
   static const int kClockSkewSeconds = 60 * 60;

   // Number of hex characters of the HMAC kept in the address (32 bits).
   static const int kHashHexLength = 8;

   int
   SRS::TimeSlotAt_(time_t when)
   {
      __int64 days = (__int64) when / 86400;
      return (int) (days % 1024);
   }

   String
   SRS::EncodeTimeSlot_(int slot)
   {
      slot = slot % 1024;
      if (slot < 0)
         slot += 1024;

      String result;
      result += kBase32Alphabet[(slot >> 5) & 31];
      result += kBase32Alphabet[slot & 31];
      return result;
   }

   bool
   SRS::DecodeTimeSlot_(const String &encoded, int &slot)
   {
      if (encoded.GetLength() != 2)
         return false;

      int high = -1;
      int low = -1;
      for (int i = 0; i < 32; i++)
      {
         wchar_t c = kBase32Alphabet[i];
         if (encoded.GetAt(0) == c) high = i;
         if (encoded.GetAt(1) == c) low = i;
      }

      if (high < 0 || low < 0)
         return false;

      slot = (high << 5) | low;
      return true;
   }

   bool
   SRS::TimeSlotInRange_(int slot, time_t now)
   {
      int currentSlot = TimeSlotAt_(now);
      int age = currentSlot - slot;
      if (age < 0)
         age += 1024;

      // The slots wrap, so the age is taken modulo the cycle: a slot that looks
      // ahead of today is one from before the wrap. Valid to the end of its
      // kMaxAgeDays-th day, by this server's clock, as it always was.
      if (age <= kMaxAgeDays)
         return true;

      // Tomorrow's, in the last hour of today. Upstream's moves the clock an hour
      // forward for the whole test, which ends every address's last day an hour
      // early; this one keeps the days as they were.
      return slot == TimeSlotAt_(now + kClockSkewSeconds);
   }

   String
   SRS::ComputeHash_(const String &secret, const String &timeSlot, const String &sourceDomain, const String &sourceLocal)
   {
      // Canonical signing string: timestamp + lowercased source domain + source
      // local-part. Both the forward and reverse paths build it identically.
      String lowerDomain = sourceDomain;
      lowerDomain.ToLower();

      // As UTF-8 (Unicode::ToUtf8Canonical) - as BATV::ComputeHash_ does, and
      // for the same reason. AnsiString(String) stopped at the first character
      // above U+00FF on Windows and wrote NUL bytes for the rest, and wrote '?'
      // for each on Linux, so two internationalized senders of one length (the
      // same up to the first such character, on Windows) shared one hash, and a
      // bounce to the SRS address of one was reversed to the other. A sender as
      // the SMTP command line gives it, its UTF-8 widened byte by byte, is hashed
      // as those bytes, as the old conversion hashed it, and so is ASCII: an SRS
      // address handed out before this change still reverses. Text UTF-8 cannot
      // carry is neither forwarded nor reversed.
      AnsiString secretBytes;
      AnsiString canonical;
      if (!Unicode::ToUtf8Canonical(secret, secretBytes) ||
          !Unicode::ToUtf8Canonical(timeSlot + lowerDomain + sourceLocal, canonical))
         return "";

      AnsiString hex = HashCreator::ComputeHMACSHA256Hex(secretBytes, canonical);
      AnsiString truncated = hex.Mid(0, kHashHexLength);

      // The hex digest is ASCII; widen it to a String for use in the address.
      String result;
      for (int i = 0; i < truncated.GetLength(); i++)
         result += (wchar_t) truncated.GetAt(i);
      return result;
   }

   String
   SRS::Forward(const String &originalMailFrom, const String &forwarderDomain, const String &secret)
   {
      return ForwardAt(originalMailFrom, forwarderDomain, secret, time(0));
   }

   String
   SRS::ForwardAt(const String &originalMailFrom, const String &forwarderDomain, const String &secret, time_t now)
   {
      if (originalMailFrom.IsEmpty() || forwarderDomain.IsEmpty() || secret.IsEmpty())
         return "";

      int atPos = originalMailFrom.ReverseFind(_T('@'));
      if (atPos <= 0 || atPos == originalMailFrom.GetLength() - 1)
         return "";

      String sourceLocal = originalMailFrom.Mid(0, atPos);
      String sourceDomain = originalMailFrom.Mid(atPos + 1);

      String timeSlot = EncodeTimeSlot_(TimeSlotAt_(now));
      String hash = ComputeHash_(secret, timeSlot, sourceDomain, sourceLocal);
      if (hash.IsEmpty())
         return "";

      String result;
      result.Format(_T("SRS0=%s=%s=%s=%s@%s"),
         hash.c_str(), timeSlot.c_str(), sourceDomain.c_str(), sourceLocal.c_str(), forwarderDomain.c_str());

      // An address no server would take is not handed out (upstream #649): longer
      // than RFC 5321 4.5.3.1.3's path allows, or a quoted local part - "john doe" -
      // carried into the unquoted SRS one, where its quotes and its space make it no
      // address at all. A bounce could never come back to either, and this server's
      // own RCPT TO would refuse one that tried; the sender is forwarded as it came.
      // Internationalized senders (RFC 6531) stay rewritable.
      if (!StringParser::IsValidEmailAddress(result, true))
         return "";

      return result;
   }

   bool
   SRS::IsSRS0(const String &address)
   {
      int atPos = address.ReverseFind(_T('@'));
      if (atPos <= 0)
         return false;

      String localPart = address.Mid(0, atPos);

      // The character following "SRS0" is a separator that may be '=', '+' or '-'.
      if (localPart.GetLength() < 5)
         return false;

      if (localPart.Mid(0, 4).CompareNoCase(_T("SRS0")) != 0)
         return false;

      wchar_t sep = localPart.GetAt(4);
      return sep == '=' || sep == '+' || sep == '-';
   }

   bool
   SRS::Reverse(const String &address, const String &secret, String &originalMailFrom)
   {
      return ReverseAt(address, secret, time(0), originalMailFrom);
   }

   bool
   SRS::ReverseAt(const String &address, const String &secret, time_t now, String &originalMailFrom)
   {
      originalMailFrom = "";

      if (secret.IsEmpty() || !IsSRS0(address))
      {
         return false;
      }

      int atPos = address.ReverseFind(_T('@'));
      String localPart = address.Mid(0, atPos);

      // Strip the "SRS0" prefix and its separator character.
      String payload = localPart.Mid(5);

      // payload = <hash>=<tt>=<source-domain>=<source-local...>
      // The source local-part may itself contain '=' characters, so only the
      // first three '=' are treated as field separators.
      int sep1 = payload.Find(_T("="));
      if (sep1 <= 0)
      {
         return false;
      }
      int sep2 = payload.Find(_T("="), sep1 + 1);
      if (sep2 <= sep1)
      {
         return false;
      }
      int sep3 = payload.Find(_T("="), sep2 + 1);
      if (sep3 <= sep2)
      {
         return false;
      }

      String hash = payload.Mid(0, sep1);
      String timeSlot = payload.Mid(sep1 + 1, sep2 - sep1 - 1);
      String sourceDomain = payload.Mid(sep2 + 1, sep3 - sep2 - 1);
      String sourceLocal = payload.Mid(sep3 + 1);

      if (hash.IsEmpty() || timeSlot.IsEmpty() || sourceDomain.IsEmpty() || sourceLocal.IsEmpty())
      {
         return false;
      }

      // Verify the HMAC signature: case-insensitive hex, compared in constant time,
      // so that how long a refusal takes does not tell a forger how much of the
      // hash was right (upstream #649).
      String expected = ComputeHash_(secret, timeSlot, sourceDomain, sourceLocal);
      if (expected.IsEmpty() || !HashCreator::HexDigestsMatch(AnsiString(expected), hash))
      {
         return false;
      }

      // Verify the timestamp is within the acceptance window.
      int slot = 0;
      if (!DecodeTimeSlot_(timeSlot, slot))
      {
         return false;
      }
      if (!TimeSlotInRange_(slot, now))
      {
         return false;
      }

      originalMailFrom = sourceLocal + _T("@") + sourceDomain;
      return true;
   }
}
