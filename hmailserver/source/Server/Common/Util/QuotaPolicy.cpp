// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "QuotaPolicy.h"

#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Persistence/PersistentServerMessage.h"
#include "../BO/ServerMessages.h"
#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../Application/EffectiveSetting.h"
#include "../Cache/CacheContainer.h"
#include "../Cache/AccountSizeCache.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   std::shared_ptr<const Domain>
   QuotaPolicy::DomainOf_(std::shared_ptr<const Account> account)
   {
      if (!account)
         return std::shared_ptr<const Domain>();

      return CacheContainer::Instance()->GetDomain(account->GetDomainID());
   }

   // The three shares are settings with levels (Application/EffectiveSetting.h):
   // the account's own value where an administrator gave it one, the domain's
   // where it holds one (its -1 is "the server's"), the server's otherwise -
   // the rule these functions held for the domain and the server before an
   // account had a level of its own.
   int
   QuotaPolicy::WarningPercent(std::shared_ptr<const Domain> domain)
   {
      return EffectiveSetting::ResolveInt(_T("quota_warning_percent"), domain, std::shared_ptr<const Account>());
   }

   int
   QuotaPolicy::WarningPercent(std::shared_ptr<const Account> account)
   {
      return EffectiveSetting::ResolveInt(_T("quota_warning_percent"), DomainOf_(account), account);
   }

   void
   QuotaPolicy::WarningText(std::shared_ptr<const Account> account, String &subject, String &body)
   {
      std::shared_ptr<const Domain> domain = DomainOf_(account);

      // The domain's words, where it has them - which is how a domain whose
      // people read another language gets its warning in that language - and
      // the server's message otherwise. Subject and text are chosen apart, so
      // a domain that sets only the text keeps the server's subject.
      subject = domain && !domain->GetQuotaWarningSubject().IsEmpty()
         ? domain->GetQuotaWarningSubject()
         : Configuration::Instance()->GetServerMessages()->GetMessage("QUOTA_WARNING_SUBJECT");

      body = domain && !domain->GetQuotaWarningText().IsEmpty()
         ? domain->GetQuotaWarningText()
         : Configuration::Instance()->GetServerMessages()->GetMessage("QUOTA_WARNING");
   }

   int
   QuotaPolicy::GracePercent(std::shared_ptr<const Domain> domain)
   {
      return EffectiveSetting::ResolveInt(_T("quota_grace_percent"), domain, std::shared_ptr<const Account>());
   }

   int
   QuotaPolicy::GracePercent(std::shared_ptr<const Account> account)
   {
      return EffectiveSetting::ResolveInt(_T("quota_grace_percent"), DomainOf_(account), account);
   }

   int
   QuotaPolicy::SendStopPercent(std::shared_ptr<const Domain> domain)
   {
      return EffectiveSetting::ResolveInt(_T("quota_send_stop_percent"), domain, std::shared_ptr<const Account>());
   }

   int
   QuotaPolicy::SendStopPercent(std::shared_ptr<const Account> account)
   {
      return EffectiveSetting::ResolveInt(_T("quota_send_stop_percent"), DomainOf_(account), account);
   }

   bool
   QuotaPolicy::AcceptsDelivery(std::shared_ptr<const Account> account, __int64 bytes)
   {
      if (!account)
         return false;

      return account->SpaceAvailable(bytes, GracePercent(account));
   }

   int
   QuotaPolicy::UsedPercent(std::shared_ptr<const Account> account)
   {
      if (!account || account->GetAccountMaxSize() <= 0)
         return 0;

      const __int64 quotaBytes = ((__int64) account->GetAccountMaxSize()) * 1024 * 1024;
      const __int64 used = AccountSizeCache::Instance()->GetSize(account->GetID());

      if (used <= 0)
         return 0;

      return (int) (used * 100 / quotaBytes);
   }

   bool
   QuotaPolicy::SendingStopped(std::shared_ptr<const Account> account, String &reason)
   {
      reason = "";

      if (!account || account->GetAccountMaxSize() <= 0)
         return false;

      const int stopAt = SendStopPercent(account);

      if (stopAt <= 0)
         return false;

      const int used = UsedPercent(account);

      if (used < stopAt)
         return false;

      reason.Format(_T("Sending is stopped: the mailbox holds %d%% of its %d MB limit, and this domain stops sending at %d%%. Delete some mail to send again."),
                    used, account->GetAccountMaxSize(), stopAt);

      return true;
   }
}
