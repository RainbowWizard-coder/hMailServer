// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "GreyListing.h"
#include "../Common/AntiSpam/AntiSpamConfiguration.h"

#include "../Common/BO/GreyListingWhiteAddresses.h"
#include "../Common/Persistence/PersistentGreyList.h"
#include "../Common/BO/GreyListTriplet.h"

#include "../Common/Util/VariantDateTime.h"
#include "../Common/Util/Time.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   GreyListing::GreyListing(void)
   {
   }

   GreyListing::~GreyListing(void)
   {
   }

   GreyListing::Verdict
   GreyListing::GetVerdict(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Implements grey listing spam protection.
   //---------------------------------------------------------------------------()
   {
      if (PersistentGreyListingWhiteAddress::IsSenderWhitelisted(remoteIP.ToString()))
      {
         // The senders IP address is white listed.
         return Allow;
      }

      // Try to read triplet.
      std::shared_ptr<GreyListTriplet> pTriplet = PersistentGreyList::GetRecord(sSenderAddress, sRecipientAddress, remoteIP);

      AntiSpamConfiguration &antiSpamConfig = Configuration::Instance()->GetAntiSpamConfiguration();

      DateTime dtNow = DateTime::GetCurrentTime();

      const int iUnblockMinutes = antiSpamConfig.GetGreyListingInitialDelay();
      const int iInitialDeleteHours = antiSpamConfig.GetGreyListingInitialDelete();

      DateTimeSpan tsUnblock;
      DateTimeSpan tsInitialDelete;
      tsUnblock.SetDateTimeSpan(0, 0, iUnblockMinutes, 0);
      tsInitialDelete.SetDateTimeSpan(0, iInitialDeleteHours, 0, 0);

      if (pTriplet)
      {
         // Check if this triplet is valid yet.
         DateTime dtTripletBlockEnd = Time::GetDateFromSystemDate(pTriplet->GetBlockEndTime());

         // The abuse threshold (discussion #282). A triplet that has never passed
         // and has retried too soon at least the threshold's number of times is
         // in its lock: refused with 550 until the lock's end, which is the
         // block end the lock set. A triplet whose lock has passed starts over
         // as a fresh first attempt rather than walking through on the strength
         // of having waited out its own refusal - the row is reset in place,
         // counters at zero, and the sender is deferred as a newcomer is.
         //
         // The passed count is what tells a locked triplet from a confirmed one
         // whose count happens to clear a threshold lowered since it passed: a
         // confirmed triplet has passed at least once, a locked one never.
         //
         // And GetIsLocked is what tells it from a pending triplet that was
         // never refused at all. The count on its own is not evidence: turning
         // the threshold on, or lowering it, finds triplets whose stored count
         // already clears it, and reading those as locked refused a sender
         // with a 550 that no lock had ever been applied to and that nothing
         // wrote a line about - or, where the window had passed, threw away a
         // wait the sender had already served. Only a row the lock was written
         // to is in the lock; anything else goes down the ordinary path below,
         // which applies the lock properly, on this attempt, and says so in
         // the log.
         const int iAbuseThreshold = antiSpamConfig.GetGreyListingAbuseThreshold();
         const bool inAbuseState = iAbuseThreshold > 0 &&
                                   pTriplet->GetPassedCount() == 0 &&
                                   pTriplet->GetBlockedCount() >= iAbuseThreshold &&
                                   pTriplet->GetIsLocked();

         if (inAbuseState)
         {
            if (dtNow <= dtTripletBlockEnd)
            {
               PersistentGreyList::IncreaseBlocked(pTriplet->GetID());
               return Reject;
            }

            if (!PersistentGreyList::ResetPending(pTriplet->GetID(),
                   Time::GetTimeStampFromDateTime(dtNow + tsUnblock),
                   Time::GetTimeStampFromDateTime(dtNow + tsInitialDelete)))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6951, "GreyListing::GetVerdict", "Failed to reset a locked grey listing triplet in the database.");
            }

            return Defer;
         }

         if (dtNow > dtTripletBlockEnd)
         {
            // Delivery is OK. We should reset the deletion time of the triple.

            const int iDeleteHours = antiSpamConfig.GetGreyListingFinalDelete();

            DateTimeSpan dtDeleteAdd;
            dtDeleteAdd.SetDateTimeSpan(0, iDeleteHours, 0,0);

            pTriplet->SetDeleteTime(Time::GetTimeStampFromDateTime(dtNow + dtDeleteAdd));

            // Save the updated triplet.
            if (!PersistentGreyList::ResetDeletionTime(pTriplet))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4327, "GreyListing::GetVerdict", "Failed to save grey listing triplet in the database.");
            }

            return Allow;
         }

         // No, the block time hasn't passed yet: a retry before the window opened.
         //
         // With the threshold on, this retry may be the one that crosses it:
         // the count after this attempt is the stored count plus one. Crossing
         // it turns the triplet into a refusal for the lock's minutes, and the
         // row's delete time moves with it so the sweep does not lift the lock
         // early. Without a threshold, or below it, the retry counts as it
         // always has - and with the slide on, it also moves the window, so the
         // wait starts again from now, with the delete time refreshed so the
         // row cannot expire in the middle of a slide.
         const int countAfterThisAttempt = pTriplet->GetBlockedCount() + 1;

         if (iAbuseThreshold > 0 && countAfterThisAttempt >= iAbuseThreshold)
         {
            const int iLockMinutes = antiSpamConfig.GetGreyListingAbuseLockMinutes();

            DateTimeSpan tsLock;
            tsLock.SetDateTimeSpan(0, 0, iLockMinutes > 0 ? iLockMinutes : 60, 0);

            const String lockEnd = Time::GetTimeStampFromDateTime(dtNow + tsLock);

            if (!PersistentGreyList::Lock(pTriplet->GetID(), lockEnd, lockEnd))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6952, "GreyListing::GetVerdict", "Failed to lock an abused grey listing triplet in the database.");
               return Defer;
            }

            // "attempts" rather than "retries", which is what the counter
            // actually holds: every RCPT TO for the triplet before its window
            // opens counts, and a sender with twenty queued messages for one
            // recipient makes twenty of them down one connection without
            // retrying anything. docs/Greylisting.md says so beside the setting.
            String logMessage;
            logMessage.Format(_T("Greylisting: %s to %s from %s made %d delivery attempts before its window opened and is refused until %s (GreyListingAbuseThreshold %d)."),
               sSenderAddress.c_str(), sRecipientAddress.c_str(), String(remoteIP.ToString()).c_str(), countAfterThisAttempt, lockEnd.c_str(), iAbuseThreshold);
            LOG_APPLICATION(logMessage);

            return Reject;
         }

         if (antiSpamConfig.GetGreyListingSlideOnEarlyRetry())
         {
            if (!PersistentGreyList::Slide(pTriplet->GetID(),
                   Time::GetTimeStampFromDateTime(dtNow + tsUnblock),
                   Time::GetTimeStampFromDateTime(dtNow + tsInitialDelete)))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6953, "GreyListing::GetVerdict", "Failed to move the window of a grey listing triplet in the database.");
            }

            return Defer;
         }

         PersistentGreyList::IncreaseBlocked(pTriplet->GetID());

         return Defer;
      }

      // Create new triplet record.
      pTriplet = std::shared_ptr<GreyListTriplet>(new GreyListTriplet());

      pTriplet->SetCreateTime(Time::GetCurrentDateTime());
      pTriplet->SetBlockEndTime(Time::GetTimeStampFromDateTime(dtNow + tsUnblock));
      pTriplet->SetDeleteTime(Time::GetTimeStampFromDateTime(dtNow + tsInitialDelete));

      pTriplet->SetIPAddress(remoteIP);
      pTriplet->SetSenderAddress(sSenderAddress);
      pTriplet->SetRecipientAddress(sRecipientAddress);

      if (!PersistentGreyList::AddObject(pTriplet))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4326, "GreyListing::GetVerdict", "Failed to save grey listing triplet in the database.");

         // Allow delivery since there appears to be internal
         // system problems.
         return Allow;
      }

      return Defer;

   }
}
