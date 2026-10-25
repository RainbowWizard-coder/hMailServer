// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class Rule;

   class PersistentRule
   {
   public:
      PersistentRule(void);
      ~PersistentRule(void);

      static bool ReadObject(std::shared_ptr<Rule> pRule, const SQLCommand& sSQL);
      static bool ReadObject(std::shared_ptr<Rule> pRule, std::shared_ptr<DALRecordset> pRS);

      static bool SaveObject(std::shared_ptr<Rule> pRule, String &errorMessage, PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<Rule> pRule);
      static bool DeleteObject(std::shared_ptr<Rule> pRule);

      // The rule's place in its list, and nothing else of it: what moving a rule
      // writes (Rules::UpdateSortOrder_). The rule in memory may hold a change that was
      // not saved - its name, its owner, a criterion or an action - and only Rule.Save
      // holds such a change to what it must pass (RuleGuard::WhyActionMayNotBeStored).
      static bool SaveSortOrder(std::shared_ptr<Rule> pRule);

      // The owner of a stored rule, as its row says: the account, the domain, or
      // neither for a global rule. False when there is no such row or it could not be
      // read. What RuleAction.Save holds an action to (RuleGuard::WhyActionMayNotBeStored).
      static bool ReadOwner(__int64 ruleId, __int64 &accountId, __int64 &domainId);

      static void DeleteByAccountID(__int64 ID);
      static void DeleteByDomainID(__int64 ID);

   private:

      static void NotifyReload_(std::shared_ptr<Rule> pRule);
      static void NotifyReload_(__int64 accountId, __int64 domainId);
   };
}