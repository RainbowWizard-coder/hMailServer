// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "QuarantineDigestTask.h"
#include "Application.h"

#include "../AntiSpam/QuarantineDigest.h"
#include "../AntiSpam/QuarantineStore.h"
#include "../BO/Account.h"
#include "../BO/Accounts.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"

#include <ctime>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How many times this task will run again before the hour is over,
      // counting the run happening now. Five minutes between runs, so twelve at
      // the top of the hour and one in its last five minutes.
      const int MinutesBetweenRuns = 5;

      // One tick's ceiling, whatever the arithmetic says. A domain with fifty
      // thousand accounts would otherwise put four thousand messages into the
      // spool in one pass, which is the storm this whole design is about; the
      // remainder simply goes in the next tick, or the next hour.
      const int MostPerPass = 500;

      // Runs since the last sweep of used and expired tokens. Started high so
      // that the first pass after a restart sweeps.
      int runs_since_sweep = 12;

      struct Due
      {
         std::shared_ptr<Account> account;
         std::shared_ptr<Domain> domain;
         std::vector<QuarantinedMessage> held;
      };

      bool SentWithinHours_(const DateTime &lastSent, int hours)
      {
         if (lastSent.GetStatus() != DateTime::valid)
            return false;

         DateTimeSpan span;
         span.SetDateTimeSpan(0, hours, 0, 0);

         DateTime cutoff = DateTime::GetCurrentTime();
         cutoff -= span;

         return cutoff < lastSent;
      }
   }

   QuarantineDigestTask::QuarantineDigestTask()
   {
   }

   QuarantineDigestTask::~QuarantineDigestTask()
   {
   }

   int
   QuarantineDigestTask::TicksLeftInHour_(int minute)
   {
      const int minutesLeft = 60 - minute;
      const int ticks = (minutesLeft + MinutesBetweenRuns - 1) / MinutesBetweenRuns;

      return ticks < 1 ? 1 : ticks;
   }

   void
   QuarantineDigestTask::DoWork()
   {
      // Housekeeping first, and once an hour: a used token is dead the moment it
      // is redeemed, and a table of dead capabilities is a table somebody will
      // one day read looking for a live one.
      if (++runs_since_sweep >= 12)
      {
         runs_since_sweep = 0;
         QuarantineDigest::Sweep();
      }

      RunPass(false);
   }

   int
   QuarantineDigestTask::RunPass(bool force)
   {
      // Nothing to hold, nothing to list. Checked first because it is the
      // shipped default, and because it makes the whole pass free on a server
      // that refuses spam rather than holding it.
      if (!QuarantineStore::GetEnabled())
         return 0;

      const bool haveBaseUrl = !QuarantineDigest::BaseUrl().IsEmpty();

      std::time_t now = std::time(nullptr);
      struct tm local;

#ifdef HM_PLATFORM_POSIX
      if (::localtime_r(&now, &local) == nullptr)
         return 0;
#else
      if (localtime_s(&local, &now) != 0)
         return 0;
#endif

      Domains domains;
      domains.Refresh();

      std::vector<Due> due;

      for (int i = 0; i < domains.GetCount(); i++)
      {
         std::shared_ptr<Domain> domain = domains.GetItem(i);

         if (!domain || !domain->GetIsActive())
            continue;

         if (!domain->GetQuarantineDigest())
            continue;

         // A digest with no link in it is a list of things the reader cannot act
         // on. Rather than send that, nothing is sent and the administrator is
         // told once - in the log, where an administrator who turned the digest
         // on and saw nothing happen will look. Only once a domain actually has
         // the digest on: a server holding spam with no digest configured has
         // nothing to be told.
         if (!haveBaseUrl)
         {
            static bool complained = false;

            if (!complained)
            {
               complained = true;
               LOG_APPLICATION(Formatter::Format("QuarantineDigest: {0} has the quarantine digest switched on, but QuarantineDigestBaseUrl is not set, so the release links would point nowhere. No digest will be sent until it is.",
                  domain->GetName()));
            }

            return 0;
         }

         if (!force && domain->GetQuarantineDigestHour() != local.tm_hour)
            continue;

         std::shared_ptr<Accounts> accounts = domain->GetAccounts();

         if (!accounts)
            continue;

         accounts->Refresh();

         for (int j = 0; j < accounts->GetCount(); j++)
         {
            std::shared_ptr<Account> account = accounts->GetItem(j);

            if (!account || !account->GetActive())
               continue;

            QuarantineDigest::State state;

            // A state row that could not be read is a state this pass does not
            // know: skipped rather than guessed, because guessing "never sent"
            // would send a digest that repeats what the reader was shown
            // yesterday.
            if (!QuarantineDigest::GetState(account->GetID(), state))
               continue;

            if (state.opted_out)
               continue;

            if (!force && SentWithinHours_(state.last_sent, 20))
               continue;

            Due entry;
            entry.account = account;
            entry.domain = domain;
            entry.held = QuarantineDigest::Held(account->GetAddress(), state.last_id);

            // The rule that keeps the feature welcome: no digest says "nothing
            // was held for you".
            if (entry.held.empty())
               continue;

            due.push_back(entry);
         }
      }

      if (due.empty())
         return 0;

      // The share of the hour this tick owes. Forced passes send the lot: an
      // administrator pressing send, or a test, is not spreading anything.
      int batch = (int) due.size();

      if (!force)
      {
         const int ticks = TicksLeftInHour_(local.tm_min);
         batch = ((int) due.size() + ticks - 1) / ticks;
      }

      if (batch > MostPerPass)
         batch = MostPerPass;

      int sent = 0;

      for (int i = 0; i < batch && i < (int) due.size(); i++)
      {
         const Due &entry = due[i];

         __int64 highestId = 0;

         if (!QuarantineDigest::SendFor(entry.account, entry.domain, entry.held, highestId))
            continue;

         // Recorded only after the message is actually in the spool. The other
         // order would mark a digest sent that was never written, and the
         // entries it would have listed would never be mentioned again.
         QuarantineDigest::RecordSent(entry.account->GetID(), highestId);

         sent++;
      }

      return sent;
   }
}
