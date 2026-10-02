// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class GreyListTriplet;

   class PersistentGreyList
   {
   public:
      PersistentGreyList(void);
      ~PersistentGreyList(void);

      // The triplet with these keys (GreyListTriplet::ComputeKey of the sender
      // and of the recipient) from this address, or null. The key is the whole
      // of the lookup: since schema 6107 every row has one and a triplet has one
      // row (PreReqGreyListKeys finished the migration 6072 began), so a miss is
      // a miss and the address columns are never asked. Null for an empty key,
      // which no row has.
      static std::shared_ptr<GreyListTriplet> GetRecordByKey(const String &senderKey, const String &recipientKey, const IPAddress &remoteIP);

      // Writes a new triplet, with its keys (computed from its addresses where it
      // has none). True when THIS call wrote the row, with the row's id set on the
      // triplet; false when it did not - another connection wrote the same
      // triplet first and the unique key refused this one, or the database
      // failed, or there is no key to write. The caller then reads the triplet
      // again: what is there is the triplet, and a refused insert is never taken
      // for a pass.
      static bool AddObject(std::shared_ptr<GreyListTriplet> pTriplet);

      static bool ResetDeletionTime(std::shared_ptr<GreyListTriplet> pTriplet);

      static void IncreaseBlocked(__int64 iTripletID);

      // The early-retry outcomes of the hardened greylisting (SMTP/GreyListing.h).
      // Each counts the retry as IncreaseBlocked does and moves the times too:
      // Slide moves the window to the triplet's new block end and delete time;
      // Lock does the same for the abuse refusal; ResetPending starts a triplet
      // whose lock has passed over again, with the counters at zero.
      static bool Slide(__int64 iTripletID, const String &blockEndTime, const String &deleteTime);
      static bool Lock(__int64 iTripletID, const String &blockEndTime, const String &deleteTime);
      static bool ResetPending(__int64 iTripletID, const String &blockEndTime, const String &deleteTime);

      static void ClearExpiredRecords();
      static void ClearAllRecords();

   private:

      static std::shared_ptr<GreyListTriplet> ReadTriplet_(std::shared_ptr<DALRecordset> pRS);
   };
}
