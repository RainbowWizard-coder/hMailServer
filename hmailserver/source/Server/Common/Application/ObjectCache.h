// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class DomainAliases;
   class Rules;

   class ObjectCache : public Singleton<ObjectCache>
   {
   public:
      ObjectCache(void);
      ~ObjectCache(void);

      void SetDomainAliasesNeedsReload();
      std::shared_ptr<DomainAliases> GetDomainAliases();

      void SetGlobalRulesNeedsReload();
      std::shared_ptr<Rules> GetGlobalRules();

      void SetAccountRulesNeedsReload(__int64 iAccountID);
      std::shared_ptr<Rules> GetAccountRules(__int64 iAccountID);

      // A domain's rules, cached as an account's are and reloaded when one
      // of them is saved.
      void SetDomainRulesNeedsReload(__int64 iDomainID);
      std::shared_ptr<Rules> GetDomainRules(__int64 iDomainID);

      void ClearRuleCaches();

   private:

      std::shared_ptr<DomainAliases> domain_aliases_;
      bool domain_aliases_needs_reload_;

      std::shared_ptr<Rules> global_rules_;
      bool global_rules_needs_reload_;

      std::map<__int64, std::shared_ptr<Rules> > account_rules_;
      std::set<__int64> account_rules_to_refresh_;
      std::map<__int64, std::shared_ptr<Rules> > domain_rules_;
      std::set<__int64> domain_rules_to_refresh_;

      boost::recursive_mutex domain_aliases_mutex_;
      boost::recursive_mutex global_rules_mutex_;
      boost::recursive_mutex account_rules_mutex_;
      boost::recursive_mutex domain_rules_mutex_;

   };
}