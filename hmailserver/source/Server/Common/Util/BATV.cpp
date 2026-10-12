// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "BATV.h"

#include "Hashing/HashCreator.h"
#include "Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   // The single key number used for the "K" field. Key rotation is out of scope;
   // a fixed value keeps the prvs tag value a constant 10 characters.
   static const wchar_t kKeyNumber = L'1';

   // How many days a prvs tag remains valid. Bounces normally return within
   // hours, so a short window keeps the backscatter rejection tight.
   static const int kMaxAgeDays = 7;
   static const int kDayModulus = 1000;

   // The tolerance for a clock - another node's sharing the secret - a little
   // ahead of this one's: tomorrow's day is taken in the last hour of today, and
   // at no other time, where any tag that read one day ahead was taken - which
   // modulo the 1000-day cycle is also a tag 999 days old (the shape upstream #649
   // found in SRS's timestamps).
   static const int kClockSkewSeconds = 60 * 60;

   // Number of hex characters of the HMAC kept in the tag (24 bits).
   static const int kHashHexLength = 6;

   // Fixed length of the tag value: K (1) + DDD (3) + SSSSSS (6).
   static const int kTagValueLength = 1 + 3 + kHashHexLength; // = 10

   int
   BATV::DayNumberAt_(time_t when)
   {
      __int64 days = (__int64) when / 86400;
      return (int) (days % kDayModulus);
   }

   bool
   BATV::DayNumberInRange_(int dayNumber, time_t now)
   {
      int currentDay = DayNumberAt_(now);
      int age = currentDay - dayNumber;
      if (age < 0)
         age += kDayModulus;

      // The day numbers wrap, so the age is taken modulo the cycle: a day that
      // looks ahead of today is one from before the wrap. Valid to the end of its
      // kMaxAgeDays-th day, by this server's clock, as it always was.
      if (age <= kMaxAgeDays)
         return true;

      // Tomorrow's, in the last hour of today; see SRS::TimeSlotInRange_.
      return dayNumber == DayNumberAt_(now + kClockSkewSeconds);
   }

   String
   BATV::ComputeHash_(const String &secret, const String &keyAndDay, const String &mailbox)
   {
      // Canonical signing string: the key+day prefix followed by the lowercased
      // mailbox. Both the sign and verify paths build it identically.
      String lowerMailbox = mailbox;
      lowerMailbox.ToLower();

      // As UTF-8, every character of it (Unicode::ToUtf8Canonical). AnsiString
      // (String) stopped at the first character above U+00FF on Windows and wrote
      // NUL bytes for the rest, and wrote '?' for each on Linux, so two
      // internationalized mailboxes of one length - the same letters up to the
      // first such character on Windows - shared one tag, and a bounce to one was
      // taken as a bounce to the other. A mailbox as the SMTP command line gives
      // it - its UTF-8 widened byte by byte, which is what Sign and Verify are
      // handed - is hashed as those bytes, which is what the old conversion
      // hashed, so a tag made before this change still verifies; so does every
      // tag over ASCII. Text UTF-8 cannot carry (an unpaired surrogate) is not
      // signed and never verifies.
      AnsiString secretBytes;
      AnsiString canonical;
      if (!Unicode::ToUtf8Canonical(secret, secretBytes) || !Unicode::ToUtf8Canonical(keyAndDay + lowerMailbox, canonical))
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
   BATV::Sign(const String &mailbox, const String &secret)
   {
      return SignAt(mailbox, secret, time(0));
   }

   String
   BATV::SignAt(const String &mailbox, const String &secret, time_t now)
   {
      if (mailbox.IsEmpty() || secret.IsEmpty())
         return "";

      // Never double-sign an already-tagged address.
      if (IsPrvs(mailbox))
         return "";

      int atPos = mailbox.ReverseFind(_T('@'));
      if (atPos <= 0 || atPos == mailbox.GetLength() - 1)
         return "";

      String localPart = mailbox.Mid(0, atPos);
      String domain = mailbox.Mid(atPos + 1);

      String dayDigits;
      dayDigits.Format(_T("%03d"), DayNumberAt_(now));

      String keyAndDay;
      keyAndDay += kKeyNumber;
      keyAndDay += dayDigits;

      String hash = ComputeHash_(secret, keyAndDay, mailbox);
      if (hash.IsEmpty())
         return "";

      String result;
      result.Format(_T("prvs=%s%s=%s@%s"),
         keyAndDay.c_str(), hash.c_str(), localPart.c_str(), domain.c_str());
      return result;
   }

   bool
   BATV::IsPrvs(const String &address)
   {
      int atPos = address.ReverseFind(_T('@'));
      if (atPos <= 0)
         return false;

      String localPart = address.Mid(0, atPos);

      // "prvs=" (5) + 10-char tag value + "=" + at least one character.
      if (localPart.GetLength() < 5 + kTagValueLength + 2)
         return false;

      if (localPart.Mid(0, 5).CompareNoCase(_T("prvs=")) != 0)
         return false;

      // The separator after the fixed-length tag value must be '='.
      return localPart.GetAt(5 + kTagValueLength) == _T('=');
   }

   bool
   BATV::Verify(const String &address, const String &secret, String &originalMailbox)
   {
      return VerifyAt(address, secret, time(0), originalMailbox);
   }

   bool
   BATV::VerifyAt(const String &address, const String &secret, time_t now, String &originalMailbox)
   {
      originalMailbox = "";

      if (secret.IsEmpty() || !IsPrvs(address))
         return false;

      int atPos = address.ReverseFind(_T('@'));
      String localPart = address.Mid(0, atPos);
      String domain = address.Mid(atPos + 1);

      // localPart = "prvs=" <K><DDD><SSSSSS> "=" <original-local-part>
      String payload = localPart.Mid(5);
      String tagValue = payload.Mid(0, kTagValueLength);
      // payload[kTagValueLength] is the '=' separator (validated by IsPrvs).
      String originalLocal = payload.Mid(kTagValueLength + 1);

      if (originalLocal.IsEmpty())
         return false;

      String keyAndDay = tagValue.Mid(0, 4); // K + DDD
      String hash = tagValue.Mid(4);         // SSSSSS

      String dayDigits = keyAndDay.Mid(1, 3);
      for (int i = 0; i < dayDigits.GetLength(); i++)
      {
         if (dayDigits.GetAt(i) < L'0' || dayDigits.GetAt(i) > L'9')
            return false;
      }

      String candidateMailbox = originalLocal + _T("@") + domain;

      // Verify the HMAC signature: case-insensitive hex, compared in constant time,
      // so that how long a refusal takes does not tell a forger how much of the
      // signature was right.
      String expected = ComputeHash_(secret, keyAndDay, candidateMailbox);
      if (expected.IsEmpty() || !HashCreator::HexDigestsMatch(AnsiString(expected), hash))
         return false;

      int dayNumber = (dayDigits.GetAt(0) - L'0') * 100 +
                      (dayDigits.GetAt(1) - L'0') * 10 +
                      (dayDigits.GetAt(2) - L'0');
      if (!DayNumberInRange_(dayNumber, now))
         return false;

      originalMailbox = candidateMailbox;
      return true;
   }
}
