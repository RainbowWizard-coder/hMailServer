// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "InboxIDCache.h"

#include <map>
#include <mutex>

namespace HM
{
   class Domain;
   class Account;
   class Alias;
   class DistributionList;
   class CacheConfiguration;

   class CacheContainer : public Singleton<CacheContainer>
   {
   public:
      CacheContainer(void);
      ~CacheContainer(void);

      std::shared_ptr<const Account> GetAccount(const String &sName);
      std::shared_ptr<const Account> GetAccount(__int64 iID);
      size_t GetAccountCacheSize();
      void SetAccountCacheMaxSize(size_t max_size);
      size_t GetAccountCacheMaxSize();

      std::shared_ptr<const Domain> GetDomain(const String &sName);
      std::shared_ptr<const Domain> GetDomain(__int64 iID);
      void RemoveDomain(std::shared_ptr<Domain> domain);
      size_t GetDomainCacheSize();
      void SetDomainCacheMaxSize(size_t max_size);
      size_t GetDomainCacheMaxSize();

      std::shared_ptr<const Alias> GetAlias(const String &sName);
      std::shared_ptr<const Alias> GetAlias(__int64 iID);
      size_t GetAliasCacheSize();
      void SetAliasCacheMaxSize(size_t max_size);
      size_t GetAliasCacheMaxSize();

      std::shared_ptr<const DistributionList> GetDistributionList(const String &sName);
      std::shared_ptr<const DistributionList> GetDistributionList(__int64 iID);
      size_t GetDistributionListCacheSize();
      void SetDistributionListCacheMaxSize(size_t max_size);
      size_t GetDistributionListCacheMaxSize();

      void OnPropertyChanged(std::shared_ptr<Property> pProperty);

      InboxIDCache &GetInboxIDCache();

      // One owner's rows of hm_settingoverrides (Persistence/PersistentSettingOverride.h,
      // Application/EffectiveSetting.h), name to value: a domain's (scope 1) or an
      // account's (scope 2) own values for the settings that may be set below the
      // server. Read from the database on a miss and kept - an owner with no rows
      // as well, which is nearly every owner, so that resolving a setting costs a
      // query once per owner and not once per message. Every write through
      // PersistentSettingOverride removes the owner's entry, and an entry is read
      // again after a minute so that a row another node wrote is seen. Null when
      // the database did not answer: the caller treats the level as inheriting.
      std::shared_ptr<const std::map<String, String> > GetSettingOverrides(int scope, __int64 ownerId);
      void RemoveSettingOverrides(int scope, __int64 ownerId);
      void ClearSettingOverrides();

      void Clear();
   private:

      struct SettingOverrideEntry
      {
         std::shared_ptr<const std::map<String, String> > values;
         time_t loaded_at;
      };

      // Guards the two below. Never held across a database read: a miss reads
      // with it released and stores the answer only if no write has been told
      // of since the read began (the generation), so a write racing a read is
      // never overwritten by what the read saw before it.
      std::mutex setting_overrides_mutex_;
      std::map<std::pair<int, __int64>, SettingOverrideEntry> setting_overrides_;
      unsigned __int64 setting_overrides_generation_;

      InboxIDCache inbox_idcache_;
   };
}