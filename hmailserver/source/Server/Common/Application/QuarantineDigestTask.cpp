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
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentSettingOverride.h"
#include "EffectiveSetting.h"

#include <ctime>
#include <map>
#include <set>
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
         std::shared_ptr<const Account> account;
         std::shared_ptr<const Domain> domain;
         std::vector<QuarantinedMessage> held;
      };

      bool SentWithinHours_(const DateTime &lastSent, int hours)
      {
         if (lastSent.GetStatus() != DateTime::valid)
            return false;

         DateTimeSpan span;
         span.SetDateTimeSpan(0, hours, 0, 0);

         // operator- and not -=, which DateTime does not have.
         DateTime cutoff = DateTime::GetCurrentTime() - span;

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
      return RunPass_(force, -1);
   }

   int
   QuarantineDigestTask::RunPassAtHour(int hour)
   {
      if (hour < 0 || hour > 23)
         return 0;

      return RunPass_(false, hour);
   }

   int
   QuarantineDigestTask::RunPass_(bool force, int atHour)
   {
      // Nothing to hold, nothing to list. Checked first because it is the
      // shipped default, and because it makes the whole pass free on a server
      // that refuses spam rather than holding it.
      if (!QuarantineStore::GetEnabled())
         return 0;

      // A pass told its hour is a forced pass in everything but the hour: it
      // spreads nothing and does not ask the twenty-hour rule.
      const bool spread = !force && atHour < 0;

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

      const int hour = atHour >= 0 ? atHour : local.tm_hour;

      Domains domains;
      domains.Refresh();

      // The digest switch and its hour are settings with levels: an account may
      // hold its own of either, over its domain's (Application/EffectiveSetting.h).
      // Which accounts do is read once a pass, from the cache of one name's rows
      // (never the database per pass), and kept by domain: for a domain that is
      // not at its hour, or has the digest off, those accounts are the only ones
      // that can be due, and only they are read - one account that chose another
      // hour does not make its whole domain be read at every tick. A domain
      // nobody overrode costs what it always did.
      std::map<__int64, std::set<__int64> > ownByDomain;
      std::set<__int64> domainsSwitchedOnBelow;
      std::set<__int64> ownValues;

      const TCHAR *const digestNames[] = { _T("quarantine_digest"), _T("quarantine_digest_hour") };

      for (const TCHAR *name : digestNames)
      {
         std::shared_ptr<const std::map<std::pair<int, __int64>, String> > rows =
            CacheContainer::Instance()->GetSettingOverridesNamed(name);
         if (!rows)
            continue;

         const bool isSwitch = String(name) == _T("quarantine_digest");

         for (const auto &row : *rows)
         {
            if (row.first.first != PersistentSettingOverride::ScopeAccount)
               continue;

            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(row.first.second);
            if (!account)
               continue;

            ownByDomain[account->GetDomainID()].insert(account->GetID());
            ownValues.insert(account->GetID());

            String value = row.second;
            value.Trim();
            if (isSwitch && value == _T("1"))
               domainsSwitchedOnBelow.insert(account->GetDomainID());
         }
      }

      std::vector<Due> due;

      for (int i = 0; i < domains.GetCount(); i++)
      {
         std::shared_ptr<Domain> domain = domains.GetItem(i);

         if (!domain || !domain->GetIsActive())
            continue;

         const bool domainOn = EffectiveSetting::ResolveBool(_T("quarantine_digest"), domain, std::shared_ptr<const Account>());
         auto own = ownByDomain.find(domain->GetID());
         const bool accountsOfTheirOwn = own != ownByDomain.end();

         if (!domainOn && !accountsOfTheirOwn)
            continue;

         // A digest with no link in it is a list of things the reader cannot act
         // on. Rather than send that, nothing is sent and the administrator is
         // told once - in the log, where an administrator who turned the digest
         // on and saw nothing happen will look. Only once a domain, or an account
         // of it, actually has the digest switched on: a server holding spam with
         // no digest configured has nothing to be told, and nor does a domain
         // whose only account of its own switched the digest off or chose an
         // hour under a domain that has it off.
         if (!haveBaseUrl)
         {
            if (!domainOn && domainsSwitchedOnBelow.find(domain->GetID()) == domainsSwitchedOnBelow.end())
               continue;

            static bool complained = false;

            if (!complained)
            {
               complained = true;
               LOG_APPLICATION(Formatter::Format("QuarantineDigest: {0}, or an account of it, has the quarantine digest switched on, but QuarantineDigestBaseUrl is not set, so the release links would point nowhere. No digest will be sent until it is.",
                  domain->GetName()));
            }

            return 0;
         }

         const int domainHour = EffectiveSetting::ResolveInt(_T("quarantine_digest_hour"), domain, std::shared_ptr<const Account>());

         // Every account of the domain when the domain's own digest is due now;
         // otherwise only the accounts with a switch or an hour of their own.
         const bool wholeDomain = domainOn && (force || domainHour == hour);

         if (!wholeDomain && !accountsOfTheirOwn)
            continue;

         std::vector<std::shared_ptr<const Account> > candidates;

         if (wholeDomain)
         {
            std::shared_ptr<Accounts> accounts = domain->GetAccounts();

            if (!accounts)
               continue;

            accounts->Refresh();

            for (int j = 0; j < accounts->GetCount(); j++)
               candidates.push_back(accounts->GetItem(j));
         }
         else
         {
            for (__int64 accountId : own->second)
            {
               std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(accountId);
               if (account && account->GetDomainID() == domain->GetID())
                  candidates.push_back(account);
            }
         }

         for (const std::shared_ptr<const Account> &account : candidates)
         {
            if (!account || !account->GetActive())
               continue;

            // The account's own switch and hour where it has them, its domain's
            // otherwise - read through the resolver only for the accounts the
            // overlay names; every other account's answer is its domain's.
            const bool ownValue = ownValues.find(account->GetID()) != ownValues.end();

            const bool accountOn = ownValue
               ? EffectiveSetting::ResolveBool(_T("quarantine_digest"), domain, account)
               : domainOn;

            if (!accountOn)
               continue;

            const int accountHour = ownValue
               ? EffectiveSetting::ResolveInt(_T("quarantine_digest_hour"), domain, account)
               : domainHour;

            if (!force && accountHour != hour)
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

            if (spread && SentWithinHours_(state.last_sent, 20))
               continue;

            Due entry;
            entry.account = account;
            entry.domain = domain;
            entry.held = QuarantineDigest::Held(account->GetAddress(), state.last_id, state.last_sent);

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

      if (spread)
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
