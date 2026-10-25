// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"
#include "GreyListTriplet.h"
#include "../Util/Hashing/HashCreator.h"
#include "../Util/Unicode.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Space, tab, CR and LF: the whitespace RFC 5322 folds, and the whole of
      // what the frozen normalisation trims.
      bool IsAsciiWhitespace_(wchar_t c)
      {
         return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
      }
   }

   GreyListTriplet::GreyListTriplet(void) :
      id_(0),
      passed_count_(0),
      blocked_count_(0)
   {

   }

   GreyListTriplet::~GreyListTriplet(void)
   {
   }

   void 
   GreyListTriplet::SetID(__int64 iNewID)
   {
      id_ = iNewID;
   }

   __int64 
   GreyListTriplet::GetID() const
   {
      return id_;
   }

   void 
   GreyListTriplet::SetCreateTime(const String &sNewVal)
   {
      create_time_ = sNewVal; 
   }

   String
   GreyListTriplet::GetCreateTime() const
   {
      return create_time_;
   }

   void 
   GreyListTriplet::SetBlockEndTime(const String &sNewVal)
   {
      block_end_time_ = sNewVal; 
   }

   String
   GreyListTriplet::GetBlockEndTime() const
   {
      return block_end_time_;
   }

   void 
   GreyListTriplet::SetDeleteTime(const String &sNewVal)
   {
      delete_time_ = sNewVal; 
   }

   String
   GreyListTriplet::GetDeleteTime() const
   {
      return delete_time_;
   }

   void 
   GreyListTriplet::SetIPAddress(IPAddress iNewVal)
   {
      ipaddress_ = iNewVal;
   }
   
   IPAddress 
   GreyListTriplet::GetIPAddress() const
   {
      return ipaddress_;
   }

   void 
   GreyListTriplet::SetRecipientAddress(const String &sNewVal)
   {
      recipient_address_ = sNewVal; 
   }

   String
   GreyListTriplet::GetRecipientAddress() const
   {
      return recipient_address_;
   }

   void 
   GreyListTriplet::SetSenderAddress(const String &sNewVal)
   {
      sender_address_ = sNewVal; 
   }

   String
   GreyListTriplet::GetSenderAddress() const
   {
      return sender_address_;
   }


   void 
   GreyListTriplet::SetPassedCount(int iNewVal)
   {
      passed_count_ = iNewVal;
   }

   int 
   GreyListTriplet::GetPassedCount() const
   {
      return passed_count_;
   }

   void 
   GreyListTriplet::SetBlockedCount(int iNewVal)
   {
      // blocked_count_, not passed_count_: until 18 September 2026 both of these
      // read and wrote the passed count, which nothing noticed because nothing
      // read the blocked count back - it was incremented in SQL and never
      // consulted. The abuse threshold consults it.
      blocked_count_ = iNewVal;
   }

   int
   GreyListTriplet::GetBlockedCount() const
   {
      return blocked_count_;
   }

   bool
   GreyListTriplet::GetIsLocked() const
   {
      // PersistentGreyList::Lock writes one stamp - the lock's end - to both
      // columns, and nothing else writes them equal: a pending row's expiry is
      // GreyListingInitialDelete HOURS away while its window is
      // GreyListingInitialDelay MINUTES away, and the sliding retry moves both
      // by those same two spans. (Set the delay to exactly sixty times the
      // delete's hours and the two coincide; such a row is then read as locked
      // one attempt earlier than it would have been, which costs a log line
      // and nothing else, and the configuration is already one where a triplet
      // expires the moment its window opens.)
      return !block_end_time_.IsEmpty() && block_end_time_ == delete_time_;
   }

   void
   GreyListTriplet::SetSenderHash(const String &sNewVal)
   {
      sender_hash_ = sNewVal;
   }

   String
   GreyListTriplet::GetSenderHash() const
   {
      return sender_hash_;
   }

   void
   GreyListTriplet::SetRecipientHash(const String &sNewVal)
   {
      recipient_hash_ = sNewVal;
   }

   String
   GreyListTriplet::GetRecipientHash() const
   {
      return recipient_hash_;
   }

   String
   GreyListTriplet::NormaliseAddress(const String &address)
   {
      // Step 1: ASCII whitespace off both ends. Spelled out rather than Trim(),
      // whose notion of whitespace is the C locale's and not written down anywhere
      // a reader of the documentation could check.
      int start = 0;
      int end = address.GetLength();
      while (start < end && IsAsciiWhitespace_(address[start]))
         start++;
      while (end > start && IsAsciiWhitespace_(address[end - 1]))
         end--;

      String trimmed = address.Mid(start, end - start);

      // Step 2: the domain lower-cased, the local part as it came. The last @
      // divides them, since a quoted local part may hold one.
      //
      // A to Z and nothing else, by hand. ToLower() folds through the global
      // locale, which is the process's to change and not written down anywhere
      // a reader of the documentation could check - the Turkish locale alone
      // makes I fold to a dotless i - and a normalisation that is declared
      // frozen must not depend on a setting. A domain name's letters are
      // ASCII: an internationalised one arrives here as its A-label.
      int at = trimmed.ReverseFind(_T('@'));
      if (at < 0)
         return trimmed;

      String domain = trimmed.Mid(at + 1);
      for (int i = 0; i < domain.GetLength(); i++)
      {
         if (domain[i] >= _T('A') && domain[i] <= _T('Z'))
            domain[i] = (TCHAR) (domain[i] - _T('A') + _T('a'));
      }

      return trimmed.Left(at + 1) + domain;
   }

   String
   GreyListTriplet::ComputeKey(const String &address)
   {
      // Steps 3 and 4: the UTF-8 bytes, and their MD5 as hex.
      AnsiString utf8;
      Unicode::WideToMultiByte(NormaliseAddress(address), utf8);

      HashCreator hasher(HashCreator::MD5);
      AnsiString hex = hasher.GenerateHashNoSalt(utf8, HashCreator::hex);

      return String(hex);
   }

}