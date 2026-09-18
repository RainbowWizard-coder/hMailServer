// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>

namespace HM
{
   class GreyListTriplet;

   class PersistentGreyList
   {
   public:
      PersistentGreyList(void);
      ~PersistentGreyList(void);

      // The triplet for this sender, recipient and address, or null. Looked up by
      // the fixed-width key (schema 6072) first; while rows from before that step
      // remain - their hash columns are NULL - a miss is looked up by the address
      // columns as they were written then, and a row found that way is given its
      // key and its full addresses so it is found by the key from then on.
      static std::shared_ptr<GreyListTriplet> GetRecord(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP);
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

      // Whether any row still carries no key: 1 yes, 0 no, -1 the question could
      // not be asked, which is not the same as no. Public for the diagnostics;
      // the lookup keeps its own answer and asks again only after a sweep.
      static int AnyLegacyRow();

   private:

      static std::shared_ptr<GreyListTriplet> ReadTriplet_(std::shared_ptr<DALRecordset> pRS);
      static std::shared_ptr<GreyListTriplet> FindLegacy_(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP, const String &senderKey, const String &recipientKey);
      static bool LegacyRowsMayRemain_();

      // True until a count finds no row with a NULL hash; counted at first use and
      // again after every ClearExpiredRecords, and never set true again, since
      // every row written since the step carries its key.
      static std::atomic<bool> legacy_rows_may_remain_;
      static std::atomic<bool> legacy_rows_counted_;
   };
}
