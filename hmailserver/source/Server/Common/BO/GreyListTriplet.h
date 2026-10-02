// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class GreyListTriplet
   {
   public:
      GreyListTriplet();
      ~GreyListTriplet(void);

      void SetID(__int64 iNewID);
      __int64 GetID() const;

      void SetCreateTime(const String &sNewVal);
      String GetCreateTime() const;

      void SetBlockEndTime(const String &sNewVal);
      String GetBlockEndTime() const;

      void SetDeleteTime(const String &sNewVal);
      String GetDeleteTime() const;

      void SetIPAddress(IPAddress iNewVal);
      IPAddress GetIPAddress() const;
      
      void SetSenderAddress(const String &sNewVal);
      String GetSenderAddress() const;

      void SetRecipientAddress(const String &sNewVal);
      String GetRecipientAddress() const;

      void SetPassedCount(int iNewVal);
      int GetPassedCount() const;

      void SetBlockedCount(int iNewVal);
      int GetBlockedCount() const;

      // Whether the abuse refusal was actually applied to this row: the one
      // thing that writes the same stamp to the window's end and to the row's
      // expiry is PersistentGreyList::Lock. A high blocked count is not
      // evidence of a lock - turning GreyListingAbuseThreshold on, or lowering
      // it, finds triplets whose stored count already clears it and that
      // nothing has ever refused - so this is what the abuse branch is entered
      // on.
      bool GetIsLocked() const;

      // The fixed-width half of the triplet key (schema 6072): the MD5 of the
      // normalised address as 32 lower-case hex characters, so that two addresses
      // longer than the columns used to be are two triplets. Every row has both
      // since schema 6107, whose prerequisite (PreReqGreyListKeys) gave the rows
      // from before 6072 theirs with this same function.
      void SetSenderHash(const String &sNewVal);
      String GetSenderHash() const;

      void SetRecipientHash(const String &sNewVal);
      String GetRecipientHash() const;

      // The normalisation the key is computed over, frozen and documented in
      // docs/Greylisting.md: ASCII whitespace (space, tab, CR, LF) trimmed from
      // both ends, the domain - everything after the last @ - lower-cased, the
      // local part left as received. Changing it would orphan every stored key.
      static String NormaliseAddress(const String &address);

      // MD5 of the UTF-8 bytes of NormaliseAddress(address), as lower-case hex.
      // MD5 because this is an index key and not a security boundary: 128 bits
      // separate greylist rows, and a longer digest would widen the index for no
      // operational gain. Empty only when the digest cannot be computed.
      static String ComputeKey(const String &address);

   private:

      __int64 id_;

      String create_time_;
      String block_end_time_;
      String delete_time_;

      IPAddress ipaddress_;
      String sender_address_;
      String recipient_address_;
      String sender_hash_;
      String recipient_hash_;

      int passed_count_;
      int blocked_count_;

   };
}