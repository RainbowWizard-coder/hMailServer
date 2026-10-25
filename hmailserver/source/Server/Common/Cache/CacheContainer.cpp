// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "CacheContainer.h"

#include "CacheConfiguration.h"

#include "Cache.h"

#include "../Application/PropertySet.h"
#include "../Application/Property.h"

#include "../BO/Domain.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/DistributionList.h"
#include "../BO/Group.h"

#include "../Persistence/PersistentDomain.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentAlias.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentGroup.h"
#include "../Persistence/PersistentServerMessage.h"
#include "../Persistence/PersistentSettingOverride.h"

#include "CacheReaderWithDbFallback.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   CacheContainer::CacheContainer(void) :
      setting_overrides_generation_(0),
      setting_overrides_clears_(0)
   {
      // Limit the cache of each entity to 10MB each.
      Cache<Domain>::Instance()->SetMaxSize(10 * 1024 * 1024);
      // Was a duplicated Cache<Domain>: the account cache had no size limit at
      // all, so it grew without bound until the server was restarted.
      Cache<Account>::Instance()->SetMaxSize(10 * 1024 * 1024);
      Cache<Alias>::Instance()->SetMaxSize(10 * 1024 * 1024);
      Cache<DistributionList>::Instance()->SetMaxSize(10 * 1024 * 1024);


      // When the server is stopped, the cache should be cleared.
      Application::Instance()->OnServerStopped.connect
         (
         [this]() 
            { 
               Cache<Domain>::Instance()->Clear();
               // Was a duplicated Cache<Domain>: the account cache was never
               // cleared on stop, so edits made to hm_accounts while the server
               // was stopped were ignored after it started again.
               Cache<Account>::Instance()->Clear();
               Cache<Alias>::Instance()->Clear();
               Cache<DistributionList>::Instance()->Clear();

               // And the settings overlay: a restart may be onto another
               // database, whose owners are not these.
               ClearSettingOverrides();

               // Same reasoning, for the same reason, on the set of accounts owed a
               // password-hash upgrade: it holds account ids, and the database
               // configured after a restart need not be the one those ids came from.
               PersistentAccount::ClearAllPasswordUpgradePending();
            }
      );
   }

   CacheContainer::~CacheContainer(void)
   {

   }



   std::shared_ptr<const Account> 
   CacheContainer::GetAccount(__int64 iID)
   {
      CacheReaderWithDbFallback<Account, PersistentAccount, __int64> reader;
      return reader.GetObject(iID);
   }

   std::shared_ptr<const Account>
   CacheContainer::GetAccount(const String &sName)
   {
      CacheReaderWithDbFallback<Account, PersistentAccount, String> reader;
      return reader.GetObject(sName);
   }


   size_t
   CacheContainer::GetAccountCacheSize()
   {
      return Cache<Account>::Instance()->GetSize();
   }

   void
   CacheContainer::SetAccountCacheMaxSize(size_t max_size)
   {
      return Cache<Account>::Instance()->SetMaxSize(max_size);
   }

   size_t
   CacheContainer::GetAccountCacheMaxSize()
   {
      return Cache<Account>::Instance()->GetMaxSize();
   }

   std::shared_ptr<const Domain> 
   CacheContainer::GetDomain(const String &sName)
   {
#ifdef _DEBUG
      if (sName.Contains(_T("@")))
      {
         HM_ASSERT(0);
      }
#endif
      
      CacheReaderWithDbFallback<Domain, PersistentDomain, String> reader;
      auto domain = reader.GetObject(sName);

      return domain;
   }

   std::shared_ptr<const Domain> 
   CacheContainer::GetDomain(__int64 iID)
   {
      CacheReaderWithDbFallback<Domain, PersistentDomain, __int64> reader;
      return reader.GetObject(iID);
   }
   
   size_t
   CacheContainer::GetDomainCacheSize()
   {
      return Cache<Domain>::Instance()->GetSize();
   }

   void
   CacheContainer::SetDomainCacheMaxSize(size_t max_size)
   {
      return Cache<Domain>::Instance()->SetMaxSize(max_size);
   }

   size_t
   CacheContainer::GetDomainCacheMaxSize()
   {
      return Cache<Domain>::Instance()->GetMaxSize();
   }

   void
   CacheContainer::RemoveDomain(std::shared_ptr<Domain> domain)
   {
      return Cache<Domain>::Instance()->RemoveObject(domain);
   }

   std::shared_ptr<const Alias> 
   CacheContainer::GetAlias(const String &sName)
   {
      CacheReaderWithDbFallback<Alias, PersistentAlias, String> reader;
      return reader.GetObject(sName);
   }

   std::shared_ptr<const Alias> 
   CacheContainer::GetAlias(__int64 iID)
   {
      CacheReaderWithDbFallback<Alias, PersistentAlias, __int64> reader;
      return reader.GetObject(iID);
   }

   size_t
   CacheContainer::GetAliasCacheSize()
   {
      return Cache<Alias>::Instance()->GetSize();
   }

   void
   CacheContainer::SetAliasCacheMaxSize(size_t max_size)
   {
      return Cache<Alias>::Instance()->SetMaxSize(max_size);
   }

   size_t
   CacheContainer::GetAliasCacheMaxSize()
   {
      return Cache<Alias>::Instance()->GetMaxSize();
   }

   std::shared_ptr<const DistributionList> 
   CacheContainer::GetDistributionList(const String &sName)
   {
      CacheReaderWithDbFallback<DistributionList, PersistentDistributionList, String> reader;
      return reader.GetObject(sName);
   }

   std::shared_ptr<const DistributionList> 
   CacheContainer::GetDistributionList(__int64 iID)
   {
      CacheReaderWithDbFallback<DistributionList, PersistentDistributionList, __int64> reader;
      return reader.GetObject(iID);
   }

   size_t
   CacheContainer::GetDistributionListCacheSize()
   {
      return Cache<DistributionList>::Instance()->GetSize();
   }

   void
   CacheContainer::SetDistributionListCacheMaxSize(size_t max_size)
   {
      return Cache<DistributionList>::Instance()->SetMaxSize (max_size);
   }

   size_t
   CacheContainer::GetDistributionListCacheMaxSize()
   {
      return Cache<DistributionList>::Instance()->GetMaxSize();
   }

   void 
   CacheContainer::OnPropertyChanged(std::shared_ptr<Property> pProperty)
   {
      String sPropName = pProperty->GetName();

      if (sPropName == PROPERTY_USECACHE)
      {
         bool bMainCacheEnabled = pProperty->GetBoolValue();

         Cache<Domain>::Instance()->SetEnabled(bMainCacheEnabled);
         Cache<Account>::Instance()->SetEnabled(bMainCacheEnabled);
         Cache<Alias>::Instance()->SetEnabled(bMainCacheEnabled);
         Cache<DistributionList>::Instance()->SetEnabled(bMainCacheEnabled);
      }
      else if (sPropName == PROPERTY_DOMAINCACHETTL)
         Cache<Domain>::Instance()->SetTTL(pProperty->GetLongValue());       
      else if (sPropName == PROPERTY_ACCOUNTCACHETTL)
         Cache<Account>::Instance()->SetTTL(pProperty->GetLongValue());
      else if (sPropName == PROPERTY_ALIASCACHETTL)
         Cache<Alias>::Instance()->SetTTL(pProperty->GetLongValue());
      else if (sPropName == PROPERTY_DISTRIBUTIONLISTCACHETTL)
         Cache<DistributionList>::Instance()->SetTTL(pProperty->GetLongValue());      
   }

   std::shared_ptr<const std::map<String, String> >
   CacheContainer::GetSettingOverrides(int scope, __int64 ownerId)
   {
      // A minute, after which a row written by another node - or by a script
      // straight into the table - is read. Every write this server makes is told
      // to the cache at once and does not wait for it.
      const time_t lifetime = 60;

      if (ownerId <= 0)
         return std::shared_ptr<const std::map<String, String> >(new std::map<String, String>());

      const std::pair<int, __int64> key(scope, ownerId);
      const time_t now = time(nullptr);
      unsigned __int64 generation = 0;

      {
         std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

         auto found = setting_overrides_.find(key);
         if (found != setting_overrides_.end() && now >= found->second.loaded_at && now - found->second.loaded_at < lifetime)
            return found->second.values;

         generation = setting_overrides_generation_;
      }

      std::shared_ptr<std::map<String, String> > values(new std::map<String, String>());
      if (!PersistentSettingOverride::ReadOwner(scope, ownerId, *values))
         return std::shared_ptr<const std::map<String, String> >();

      {
         std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

         if (generation == setting_overrides_generation_)
         {
            // A bound on the map, not a policy: an installation with more owners
            // than this starts again from empty rather than growing for ever.
            if (setting_overrides_.size() >= 50000)
               setting_overrides_.clear();

            SettingOverrideEntry entry;
            entry.values = values;
            entry.loaded_at = now;
            setting_overrides_[key] = entry;
         }
      }

      return values;
   }

   std::shared_ptr<const std::map<std::pair<int, __int64>, String> >
   CacheContainer::GetSettingOverridesNamed(const String &name)
   {
      const time_t lifetime = 60;

      String key = name;
      key.ToLower();

      const time_t now = time(nullptr);
      unsigned __int64 generation = 0;

      {
         std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

         auto found = setting_overrides_named_.find(key);
         if (found != setting_overrides_named_.end() && now >= found->second.loaded_at && now - found->second.loaded_at < lifetime)
            return found->second.rows;

         generation = setting_overrides_generation_;
      }

      std::shared_ptr<std::map<std::pair<int, __int64>, String> > rows(new std::map<std::pair<int, __int64>, String>());
      if (!PersistentSettingOverride::RowsOfName(key, *rows))
         return std::shared_ptr<const std::map<std::pair<int, __int64>, String> >();

      {
         std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

         if (generation == setting_overrides_generation_)
         {
            SettingOverrideNamedEntry entry;
            entry.rows = rows;
            entry.loaded_at = now;
            setting_overrides_named_[key] = entry;
         }
      }

      return rows;
   }

   void
   CacheContainer::RemoveSettingOverrides(int scope, __int64 ownerId)
   {
      std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

      setting_overrides_.erase(std::pair<int, __int64>(scope, ownerId));

      // A write to one owner changes the rows of the name it wrote, and the
      // name is not told here: every name's rows are read again. There are 27.
      setting_overrides_named_.clear();
      setting_overrides_generation_++;
   }

   void
   CacheContainer::ClearSettingOverrides()
   {
      std::lock_guard<std::mutex> guard(setting_overrides_mutex_);

      setting_overrides_.clear();
      setting_overrides_named_.clear();
      setting_overrides_generation_++;
      setting_overrides_clears_++;
   }

   InboxIDCache &
   CacheContainer::GetInboxIDCache()
   {
      return inbox_idcache_;
   }

   void 
   CacheContainer::Clear()
   {
      inbox_idcache_.Clear();
   }

}