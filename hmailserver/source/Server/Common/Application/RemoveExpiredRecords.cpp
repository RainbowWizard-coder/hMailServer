// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "RemoveExpiredRecords.h"

#include "../Persistence/PersistentSecurityRange.h"
#include "../Util/AutoBanFirewall.h"
#include "../Util/NeverBanNameList.h"
#include "../Persistence/PersistentLogonFailure.h"
#include "../AntiSpam/SpamTrapMonitor.h"
#include "../Util/ApiSends.h"
#include "../Util/MessageTrace.h"
#include "../Util/RestApiServer.h"
#include "../Util/MailboxKeys.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   RemoveExpiredRecords::RemoveExpiredRecords(void)
   {
   }

   RemoveExpiredRecords::~RemoveExpiredRecords(void)
   {
   }
   
   void
   RemoveExpiredRecords::DoWork()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Remove old records from the greylist
   //---------------------------------------------------------------------------()
   {
      // The mailbox keys of accounts encrypted at rest (Util/MailboxKeys.h)
      // that no session holds and their owner has not used for
      // MailboxKeyIdleMinutes: closed, and written over in memory. Every
      // minute, so that a key is not open for much longer than the setting
      // says - and FIRST, touching no database, so that a step below that
      // throws does not keep every idle key open for as long as it goes on
      // throwing.
      try
      {
         MailboxKeys::CloseIdle();
      }
      catch (...)
      {
         LOG_APPLICATION("RemoveExpiredRecords: the idle mailbox keys could not be closed this minute; the next run tries again.");
      }

      // This is what un-bans an address once its auto-ban expires, and its result was
      // discarded - the mirror image of the auto-ban save that was also unchecked.
      // A failure here does not fail open, it fails *closed* and stays that way: the
      // expired range keeps matching, so the address is refused indefinitely, and
      // "I can't connect and the server says nothing" is a support call nobody can
      // answer from the logs. The task runs again on its own schedule, so one failure
      // corrects itself; being told about it is what turns a permanent-looking ban
      // into a transient one.
      if (!PersistentSecurityRange::DeleteExpired())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6112, "RemoveExpiredRecords::DoWork",
            "Expired IP ranges could not be removed. Any auto-ban that has run out will keep blocking the address until this succeeds on a later run.");
      }


      // Whether or not the delete succeeded: what is in the table now is what the
      // firewall should show, and this pass is what un-bans an address below the
      // server once its range has gone.
      AutoBanFirewall::Synchronise(_T("expiry pass"));

      // The bans that wait for the never-ban list by reverse name: those whose
      // lookups were not answered asked about again, a minute a ban waited past
      // its time without a turn counted as an attempt, those out of attempts or
      // time announced, and those whose range has gone forgotten - so that a
      // resolver that was down for a minute neither announces a listed provider
      // nor holds anybody's ban below the server for long. Nothing while there
      // are none.
      try
      {
         NeverBanNameList::RetryUnverified();
      }
      catch (...)
      {
         LOG_DEBUG("RemoveExpiredRecords: the unverified auto-bans could not be asked about again this minute; the next run tries again.");
      }

      if (Configuration::Instance()->GetAutoBanLogonEnabled())
      {
         int logonFailureMinutes = Configuration::Instance()->GetMaxLogonAttemptsWithin();
         
         // Unchecked, and this one errs towards blocking: failures older than
         // MaxLogonAttemptsWithin are what stop a handful of typos spread over a week
         // from adding up to a ban. If they are never cleared they keep accumulating,
         // and a legitimate user who mistypes their password occasionally is
         // eventually banned for it.
         PersistentLogonFailure persistentLogonFailure;

         if (!persistentLogonFailure.ClearOldFailures(logonFailureMinutes))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6115, "RemoveExpiredRecords::DoWork",
               "Expired logon failures could not be cleared. They keep counting towards the auto-ban threshold until this succeeds, so occasional failures spread over a long period may add up to a ban.");
         }
      }

      // The record of spam trap hits, swept once an hour rather than on every
      // run. This task runs every minute; the record is bounded by a retention
      // period rather than by a promise about any one row, so a delete a minute
      // against a table nothing else reads would be work for its own sake.
      {
         static time_t lastSpamTrapSweep = 0;
         const time_t now = time(nullptr);

         if (lastSpamTrapSweep == 0 || now - lastSpamTrapSweep >= 3600)
         {
            lastSpamTrapSweep = now;
            SpamTrapMonitor::SweepHits();
         }
      }

      // The message trace, and the send API's record of what each key sent
      // (Util/ApiSends.h), which is answered from it: each kept
      // MessageTraceRetentionDays (0 keeps them) and swept hourly for the same
      // reason as the record above. Until 24 September 2026 nothing swept the
      // trace at all - MessageTrace::DeleteExpired ran only when somebody called
      // MessageTrace.DeleteExpired over COM - so the setting promised a window it
      // never kept. And the rows of a key revoked outside the key routes (the
      // Control Panel's revoke, an edit of the store), which only the key could
      // have read: when the store can be read, a key no longer in it loses them.
      {
         static time_t lastTraceSweep = 0;
         const time_t now = time(nullptr);

         if (lastTraceSweep == 0 || now - lastTraceSweep >= 3600)
         {
            lastTraceSweep = now;

            const int traceRemoved = MessageTrace::DeleteExpired();
            if (traceRemoved > 0)
               LOG_APPLICATION(Formatter::Format(_T("RemoveExpiredRecords: {0} message trace event(s) older than MessageTraceRetentionDays removed."), traceRemoved));

            const int removed = ApiSends::DeleteExpired();
            if (removed > 0)
               LOG_APPLICATION(Formatter::Format(_T("RemoveExpiredRecords: {0} send API record(s) older than MessageTraceRetentionDays removed."), removed));

            std::vector<String> liveKeys;
            if (RestApiServer::StoredApiKeyIds(liveKeys))
            {
               const int orphans = ApiSends::DeleteOrphans(liveKeys);
               if (orphans > 0)
                  LOG_APPLICATION(Formatter::Format(_T("RemoveExpiredRecords: {0} send API record(s) of keys no longer in the key store removed."), orphans));
            }
         }
      }
   }

}