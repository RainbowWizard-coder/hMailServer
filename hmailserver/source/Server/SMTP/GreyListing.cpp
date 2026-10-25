// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "GreyListing.h"
#include "../Common/AntiSpam/AntiSpamConfiguration.h"

#include "../Common/BO/GreyListingWhiteAddresses.h"
#include "../Common/Persistence/PersistentGreyList.h"
#include "../Common/BO/GreyListTriplet.h"

#include "../Common/Util/VariantDateTime.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/GUIDCreator.h"

#include <atomic>
#include <stdexcept>


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

      // The key is the whole of the lookup (schema 6107): every row has one, and
      // the address columns are never asked.
      const String senderKey = GreyListTriplet::ComputeKey(sSenderAddress);
      const String recipientKey = GreyListTriplet::ComputeKey(sRecipientAddress);

      if (senderKey.IsEmpty() || recipientKey.IsEmpty())
      {
         // The digest could not be computed, which a stock OpenSSL never refuses.
         // There is nothing to find a triplet by and nothing to write one with,
         // so greylisting stands aside, as it does whenever this server cannot
         // keep its own record - said once, at High, rather than once a message.
         static std::atomic<bool> reported(false);
         if (!reported.exchange(true))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 6950, "GreyListing::GetVerdict",
               "The greylisting key could not be computed - this build's OpenSSL answered no MD5 digest. "
               "Greylisting lets every recipient through until it does; nothing else can identify a triplet since schema 6107.");
         }

         return Allow;
      }

      std::shared_ptr<GreyListTriplet> pTriplet = PersistentGreyList::GetRecordByKey(senderKey, recipientKey, remoteIP);
      if (!pTriplet)
         return VerdictForUnknown_(sSenderAddress, sRecipientAddress, remoteIP, senderKey, recipientKey);

      return VerdictForKnown_(pTriplet, sSenderAddress, sRecipientAddress, remoteIP);
   }

   GreyListing::Verdict
   GreyListing::VerdictForUnknown_(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP, const String &senderKey, const String &recipientKey)
   {
      AntiSpamConfiguration &antiSpamConfig = Configuration::Instance()->GetAntiSpamConfiguration();

      DateTime dtNow = DateTime::GetCurrentTime();

      DateTimeSpan tsUnblock;
      DateTimeSpan tsInitialDelete;
      tsUnblock.SetDateTimeSpan(0, 0, antiSpamConfig.GetGreyListingInitialDelay(), 0);
      tsInitialDelete.SetDateTimeSpan(0, antiSpamConfig.GetGreyListingInitialDelete(), 0, 0);

      std::shared_ptr<GreyListTriplet> pTriplet = std::shared_ptr<GreyListTriplet>(new GreyListTriplet());

      pTriplet->SetCreateTime(Time::GetCurrentDateTime());
      pTriplet->SetBlockEndTime(Time::GetTimeStampFromDateTime(dtNow + tsUnblock));
      pTriplet->SetDeleteTime(Time::GetTimeStampFromDateTime(dtNow + tsInitialDelete));

      pTriplet->SetIPAddress(remoteIP);
      pTriplet->SetSenderAddress(sSenderAddress);
      pTriplet->SetRecipientAddress(sRecipientAddress);
      pTriplet->SetSenderHash(senderKey);
      pTriplet->SetRecipientHash(recipientKey);

      if (PersistentGreyList::AddObject(pTriplet))
         return Defer;

      // Not written. Either the key's unique index refused it - another
      // connection presented the same triplet a moment ago and wrote it after
      // this one looked - or the database failed. Whatever is there now is the
      // triplet, and it is answered as one: a race must not be a way past
      // greylisting, and must not defer a confirmed sender either.
      std::shared_ptr<GreyListTriplet> existing = PersistentGreyList::GetRecordByKey(senderKey, recipientKey, remoteIP);
      if (existing)
      {
         LOG_DEBUG(Formatter::Format("Greylisting: {0} to {1} from {2} was written by another connection between this one's lookup and its insert; answered as that triplet.",
            sSenderAddress, sRecipientAddress, String(remoteIP.ToString())));
         return VerdictForKnown_(existing, sSenderAddress, sRecipientAddress, remoteIP);
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4326, "GreyListing::GetVerdict", "Failed to save grey listing triplet in the database.");

      // Allow delivery since there appears to be internal
      // system problems.
      return Allow;
   }

   GreyListing::Verdict
   GreyListing::VerdictForKnown_(std::shared_ptr<GreyListTriplet> pTriplet, const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP)
   {
      AntiSpamConfiguration &antiSpamConfig = Configuration::Instance()->GetAntiSpamConfiguration();

      DateTime dtNow = DateTime::GetCurrentTime();

      const int iUnblockMinutes = antiSpamConfig.GetGreyListingInitialDelay();
      const int iInitialDeleteHours = antiSpamConfig.GetGreyListingInitialDelete();

      DateTimeSpan tsUnblock;
      DateTimeSpan tsInitialDelete;
      tsUnblock.SetDateTimeSpan(0, 0, iUnblockMinutes, 0);
      tsInitialDelete.SetDateTimeSpan(0, iInitialDeleteHours, 0, 0);

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

   // ---- the tester -----------------------------------------------------------

   namespace
   {
      void GreyFail_(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(sentence).c_str()));
      }

      void GreyExpect_(bool condition, const String &sentence)
      {
         if (!condition)
            GreyFail_(sentence);
      }

      // One row of the tester's own triplet, as a connection that won the race
      // left it: passed `passed` times, its window `windowMinutes` from now
      // (negative: already open).
      __int64 WriteTriplet_(const String &sender, const String &recipient, const IPAddress &address, int passed, int windowMinutes)
      {
         DateTime now = DateTime::GetCurrentTime();
         DateTimeSpan window;
         window.SetDateTimeSpan(0, 0, windowMinutes < 0 ? -windowMinutes : windowMinutes, 0);
         DateTimeSpan day;
         day.SetDateTimeSpan(1, 0, 0, 0);

         std::shared_ptr<GreyListTriplet> triplet(new GreyListTriplet());
         triplet->SetCreateTime(Time::GetCurrentDateTime());
         triplet->SetBlockEndTime(Time::GetTimeStampFromDateTime(windowMinutes < 0 ? now - window : now + window));
         triplet->SetDeleteTime(Time::GetTimeStampFromDateTime(now + day));
         triplet->SetIPAddress(address);
         triplet->SetSenderAddress(sender);
         triplet->SetRecipientAddress(recipient);

         if (!PersistentGreyList::AddObject(triplet) || triplet->GetID() <= 0)
            GreyFail_(_T("The tester could not write its triplet, or was not handed its id."));

         if (passed > 0)
         {
            SQLCommand confirm("update hm_greylisting_triplets set glpassedcount = @PASSED where glid = @GLID");
            confirm.AddParameter("@PASSED", passed);
            confirm.AddParameter("@GLID", triplet->GetID());
            if (!Application::Instance()->GetDBManager()->Execute(confirm))
               GreyFail_(_T("The tester could not confirm its triplet."));
         }

         return triplet->GetID();
      }

      __int64 CountRows_(const String &senderKey, const String &recipientKey)
      {
         SQLCommand command("select count(*) as copies from hm_greylisting_triplets where glsenderhash = @SENDERHASH and glrecipienthash = @RECIPIENTHASH");
         command.AddParameter("@SENDERHASH", senderKey);
         command.AddParameter("@RECIPIENTHASH", recipientKey);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            GreyFail_(_T("The tester could not count its rows."));
         return recordset->GetInt64Value("copies");
      }

      void DeleteRows_(const String &senderKey)
      {
         SQLCommand command("delete from hm_greylisting_triplets where glsenderhash = @SENDERHASH");
         command.AddParameter("@SENDERHASH", senderKey);
         Application::Instance()->GetDBManager()->Execute(command);
      }
   }

   void
   GreyListingTester::Test()
   {
      // A sender of the tester's own, so that no row of the bench's is touched,
      // from an address of TEST-NET-3 (203.0.113.77), which no fixture uses.
      const String sender = _T("greylisting-tester-") + GUIDCreator::GetGUID() + _T("@example.test");
      const String recipient = _T("recipient@example.test");
      const IPAddress address((__int64) 3405803853LL);
      const String senderKey = GreyListTriplet::ComputeKey(sender);
      const String recipientKey = GreyListTriplet::ComputeKey(recipient);

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();
      const int threshold = config.GetGreyListingAbuseThreshold();

      GreyListing greyListing;

      try
      {
         // The insert itself. A new triplet is written and handed its id; the same
         // triplet again is refused by the unique key, and the refusal is told
         // from a write by the id - the decision the race below rests on.
         std::shared_ptr<GreyListTriplet> first(new GreyListTriplet());
         first->SetCreateTime(Time::GetCurrentDateTime());
         first->SetBlockEndTime(Time::GetCurrentDateTime());
         first->SetDeleteTime(Time::GetCurrentDateTime());
         first->SetIPAddress(address);
         first->SetSenderAddress(sender);
         first->SetRecipientAddress(recipient);
         GreyExpect_(PersistentGreyList::AddObject(first) && first->GetID() > 0, _T("A new triplet was not written, or not handed its id."));

         std::shared_ptr<GreyListTriplet> second(new GreyListTriplet());
         second->SetCreateTime(Time::GetCurrentDateTime());
         second->SetBlockEndTime(Time::GetCurrentDateTime());
         second->SetDeleteTime(Time::GetCurrentDateTime());
         second->SetIPAddress(address);
         second->SetSenderAddress(sender);
         second->SetRecipientAddress(recipient);
         GreyExpect_(!PersistentGreyList::AddObject(second), _T("A second row of one triplet was reported as written."));
         GreyExpect_(CountRows_(senderKey, recipientKey) == 1, _T("A triplet has two rows: the key is not unique on this database."));
         DeleteRows_(senderKey);

         // The race, with its outcome fixed: the lookup has missed, and another
         // connection has written the triplet since. Confirmed there, it is a
         // confirmed triplet here - let through, its pass counted. (Before schema
         // 6107 the refused insert read as a write and this answered 451, and on
         // PostgreSQL before 15 September 2026 it read as a failure and let the
         // recipient through whatever the triplet was.)
         const __int64 confirmed = WriteTriplet_(sender, recipient, address, 1, -60);
         GreyExpect_(greyListing.VerdictForUnknown_(sender, recipient, address, senderKey, recipientKey) == GreyListing::Allow,
            _T("A confirmed triplet another connection wrote was not answered as confirmed."));
         std::shared_ptr<GreyListTriplet> afterAllow = PersistentGreyList::GetRecordByKey(senderKey, recipientKey, address);
         GreyExpect_(afterAllow && afterAllow->GetID() == confirmed && afterAllow->GetPassedCount() == 2,
            _T("The confirmed triplet's pass was not counted on its own row."));
         GreyExpect_(CountRows_(senderKey, recipientKey) == 1, _T("The race left two rows."));
         DeleteRows_(senderKey);

         // Pending there, it is a retry before the window here: deferred, and
         // counted as the attempt it is.
         const __int64 pending = WriteTriplet_(sender, recipient, address, 0, 60);
         const GreyListing::Verdict expected = (threshold > 0 && threshold <= 1) ? GreyListing::Reject : GreyListing::Defer;
         GreyExpect_(greyListing.VerdictForUnknown_(sender, recipient, address, senderKey, recipientKey) == expected,
            _T("A pending triplet another connection wrote was let through, or not answered as pending."));
         std::shared_ptr<GreyListTriplet> afterDefer = PersistentGreyList::GetRecordByKey(senderKey, recipientKey, address);
         GreyExpect_(afterDefer && afterDefer->GetID() == pending && afterDefer->GetBlockedCount() == 1 && afterDefer->GetPassedCount() == 0,
            _T("The attempt on a pending triplet another connection wrote was not counted on its row."));
         GreyExpect_(CountRows_(senderKey, recipientKey) == 1, _T("The race left two rows."));
         DeleteRows_(senderKey);

         // And with nothing there, the ordinary first attempt.
         GreyExpect_(greyListing.VerdictForUnknown_(sender, recipient, address, senderKey, recipientKey) == GreyListing::Defer,
            _T("A first attempt was not deferred."));
         std::shared_ptr<GreyListTriplet> made = PersistentGreyList::GetRecordByKey(senderKey, recipientKey, address);
         GreyExpect_(made && made->GetBlockedCount() == 0 && made->GetPassedCount() == 0 && made->GetSenderAddress() == sender,
            _T("A first attempt did not write its triplet."));
         GreyExpect_(CountRows_(senderKey, recipientKey) == 1, _T("A first attempt wrote two rows."));
      }
      catch (...)
      {
         DeleteRows_(senderKey);
         throw;
      }

      DeleteRows_(senderKey);
   }
}
