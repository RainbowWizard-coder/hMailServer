// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See DiscoveryHolds.h: the live holds, read into memory, and the questions every
// deletion path asks of them.

#include "StdAfx.h"
#include "DiscoveryHolds.h"

#include "../Application/Application.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // How old the copy may grow before it is read again, for a change this
      // process did not make: a hold placed or released by hand in the table,
      // or by a second server on the same database. Two queries every five
      // seconds at most, and only while something is being deleted.
      const std::time_t MaxAgeSeconds = 5;

      // How often a copy that could not be read is said to the error log.
      const std::time_t ReportEverySeconds = 60;

      boost::recursive_mutex holds_mutex;
      bool holds_loaded = false;
      bool holds_failed = false;
      std::time_t holds_loaded_at = 0;
      std::time_t holds_failure_reported_at = 0;

      std::set<__int64> held_accounts;          // scope 1: account ids
      std::set<__int64> held_account_domains;   // the domain ids of those accounts
      std::set<String> held_addresses;          // their addresses, lower-cased
      std::set<__int64> held_domains;           // scope 2: domain ids
      std::set<String> held_domain_names;       // their names, lower-cased
      std::set<__int64> held_archive_entries;   // scope 3: hm_archiveindex ids

      // DiscoveryHolds::UndoingCreation, per thread.
      thread_local int undoing_creation = 0;

      String Lower(const String &value)
      {
         String lowered = value;
         lowered.ToLower();
         return lowered;
      }
   }

   void
   DiscoveryHolds::EnsureLoaded_()
   {
      const std::time_t now = std::time(nullptr);

      if (holds_loaded && now - holds_loaded_at < MaxAgeSeconds)
         return;

      std::set<__int64> accounts, accountDomains, domains, archives;
      std::set<String> addresses, domainNames;

      bool read = false;

      SQLCommand holds(_T("select holdscope, holdownerid, holdname from hm_discoveryholds where holdreleased = 0"));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(holds);

      if (recordset)
      {
         read = true;

         while (!recordset->IsEOF())
         {
            const long scope = recordset->GetLongValue("holdscope");
            const __int64 owner = recordset->GetInt64Value("holdownerid");
            const String name = Lower(recordset->GetStringValue("holdname"));

            if (scope == ScopeAccount)
            {
               accounts.insert(owner);
               if (!name.IsEmpty())
                  addresses.insert(name);
            }
            else if (scope == ScopeDomain)
            {
               domains.insert(owner);
               if (!name.IsEmpty())
                  domainNames.insert(name);
            }
            else if (scope == ScopeArchive)
            {
               archives.insert(owner);
            }

            recordset->MoveNext();
         }
      }

      // The domains the held accounts are in, and their addresses as they are
      // now: a domain is not deleted while an account in it is held, and an
      // archived copy is filed under the address it was delivered to.
      if (read && !accounts.empty())
      {
         SQLCommand owners(_T("select a.accountid, a.accountdomainid, a.accountaddress from hm_accounts a ")
                           _T("inner join hm_discoveryholds h on h.holdownerid = a.accountid ")
                           _T("where h.holdscope = 1 and h.holdreleased = 0"));
         std::shared_ptr<DALRecordset> accountRows = Application::Instance()->GetDBManager()->OpenRecordset(owners);

         if (!accountRows)
         {
            read = false;
         }
         else
         {
            while (!accountRows->IsEOF())
            {
               accountDomains.insert(accountRows->GetInt64Value("accountdomainid"));
               addresses.insert(Lower(accountRows->GetStringValue("accountaddress")));
               accountRows->MoveNext();
            }
         }
      }

      // The held domains' names as they are now, beside the name the hold
      // recorded: the archive files a copy under the domain's name at the time
      // it archived it, so a domain renamed since its hold was placed has its
      // new copies filed under a name the hold row does not carry.
      if (read && !domains.empty())
      {
         SQLCommand names(_T("select d.domainname from hm_domains d ")
                          _T("inner join hm_discoveryholds h on h.holdownerid = d.domainid ")
                          _T("where h.holdscope = 2 and h.holdreleased = 0"));
         std::shared_ptr<DALRecordset> domainRows = Application::Instance()->GetDBManager()->OpenRecordset(names);

         if (!domainRows)
         {
            read = false;
         }
         else
         {
            while (!domainRows->IsEOF())
            {
               domainNames.insert(Lower(domainRows->GetStringValue("domainname")));
               domainRows->MoveNext();
            }
         }
      }

      if (!read)
      {
         // Everything is answered as held until the next attempt, five seconds
         // on; the error log hears of it once a minute at most.
         holds_failed = true;
         holds_loaded = true;
         holds_loaded_at = now;

         if (now - holds_failure_reported_at >= ReportEverySeconds)
         {
            holds_failure_reported_at = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7332, "DiscoveryHolds::EnsureLoaded_",
               "The legal holds (hm_discoveryholds) could not be read, so every account and every archived copy is treated as held until they can be: a delete keeps a copy, and an account, a domain or a mailbox's messages are not deleted.");
         }

         return;
      }

      held_accounts.swap(accounts);
      held_account_domains.swap(accountDomains);
      held_addresses.swap(addresses);
      held_domains.swap(domains);
      held_domain_names.swap(domainNames);
      held_archive_entries.swap(archives);

      holds_failed = false;
      holds_loaded = true;
      holds_loaded_at = now;
   }

   void
   DiscoveryHolds::Invalidate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      holds_loaded = false;
   }

   bool
   DiscoveryHolds::IsAccountHeld(__int64 accountId)
   {
      if (accountId <= 0)
         return false;

      __int64 domainId = 0;

      {
         boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
         EnsureLoaded_();

         if (holds_failed)
            return true;

         if (held_accounts.find(accountId) != held_accounts.end())
            return true;

         // The common case - no hold on any domain - asks nothing more.
         if (held_domains.empty())
            return false;
      }

      // Outside the lock: the cache may read the database. An account that
      // cannot be read while a domain hold stands is answered held, as a hold
      // that cannot be read is: which domain it is in is the question, and a
      // delete must not be the answer to not knowing.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(accountId);
      if (!account)
         return true;

      domainId = account->GetDomainID();

      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      return held_domains.find(domainId) != held_domains.end();
   }

   bool
   DiscoveryHolds::AnyAccountOrDomainHeld(String &held)
   {
      held = String();

      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      EnsureLoaded_();

      if (holds_failed)
         return true;

      if (held_accounts.empty() && held_domains.empty())
         return false;

      // The names the holds carry: the domains, then the addresses. A handful
      // is enough for the sentence; the case's own holds list them all.
      const int MaxNamed = 5;
      int named = 0;
      int more = 0;

      for (const String &domain : held_domain_names)
      {
         if (named < MaxNamed)
         {
            held += (named > 0 ? String(_T(", ")) : String()) + domain;
            named++;
         }
         else
         {
            more++;
         }
      }

      for (const String &address : held_addresses)
      {
         if (named < MaxNamed)
         {
            held += (named > 0 ? String(_T(", ")) : String()) + address;
            named++;
         }
         else
         {
            more++;
         }
      }

      if (more > 0)
         held += Formatter::Format(" and {0} more", more);

      // Never empty for a hold that was read: an empty name is the sentence for
      // holds that could not be read (RestoreRefusal).
      if (held.IsEmpty())
         held = _T("an account or a domain on this server");

      return true;
   }

   bool
   DiscoveryHolds::ReadFailed()
   {
      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      EnsureLoaded_();
      return holds_failed;
   }

   DiscoveryHolds::UndoingCreation::UndoingCreation()
   {
      undoing_creation++;
   }

   DiscoveryHolds::UndoingCreation::~UndoingCreation()
   {
      undoing_creation--;
   }

   bool
   DiscoveryHolds::UndoingCreation::Active()
   {
      return undoing_creation > 0;
   }

   bool
   DiscoveryHolds::IsDomainOrAnyAccountHeld(__int64 domainId)
   {
      if (domainId <= 0)
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      EnsureLoaded_();

      if (holds_failed)
         return true;

      return held_domains.find(domainId) != held_domains.end() ||
             held_account_domains.find(domainId) != held_account_domains.end();
   }

   bool
   DiscoveryHolds::IsArchiveEntryHeld(__int64 archiveId, const String &domain, const String &mailbox)
   {
      boost::lock_guard<boost::recursive_mutex> guard(holds_mutex);
      EnsureLoaded_();

      if (holds_failed)
         return true;

      if (archiveId > 0 && held_archive_entries.find(archiveId) != held_archive_entries.end())
         return true;

      // An Inbound copy - a sender from outside - names no domain and no
      // mailbox, and is held by its own hold alone.
      if (domain.IsEmpty())
         return false;

      const String domainLower = Lower(domain);

      if (held_domain_names.find(domainLower) != held_domain_names.end())
         return true;

      if (mailbox.IsEmpty())
         return false;

      return held_addresses.find(Lower(mailbox) + _T("@") + domainLower) != held_addresses.end();
   }

   String
   DiscoveryHolds::DeleteAccountRefusal(const String &address)
   {
      return Formatter::Format("The account {0} is under a legal hold, so it cannot be deleted until every hold on it, and on its domain, is released (docs/Discovery.md).", address);
   }

   String
   DiscoveryHolds::DeleteDomainRefusal(const String &domain)
   {
      return Formatter::Format("The domain {0}, or an account in it, is under a legal hold, so the domain cannot be deleted until every such hold is released (docs/Discovery.md).", domain);
   }

   String
   DiscoveryHolds::DeleteMessagesRefusal(const String &address)
   {
      return Formatter::Format("The account {0} is under a legal hold, so its messages cannot all be deleted at once until the hold is released (docs/Discovery.md).", address);
   }

   String
   DiscoveryHolds::ReplaceRefusal(const String &address)
   {
      return Formatter::Format("The account {0} is under a legal hold, so a restore that replaces its mailbox is refused: replace deletes what is in the mailbox now. Restore with merge, which deletes nothing, or release the hold first (docs/Discovery.md).", address);
   }

   String
   DiscoveryHolds::PurgeRefusal()
   {
      return _T("This account is under a legal hold, so its deleted items are kept until the hold is released and cannot be deleted for good before then.");
   }

   String
   DiscoveryHolds::MoveRefusal(const String &address, const String &domain)
   {
      return Formatter::Format("The account {0} is under a legal hold, on it or on its domain, so it cannot be moved to {1} until every such hold is released: a hold on a domain holds the accounts in that domain, and one moved out of it would leave the hold behind (docs/Discovery.md).", address, domain);
   }

   String
   DiscoveryHolds::RestoreRefusal(const String &held)
   {
      if (held.IsEmpty())
         return _T("Restore refused: a restore with the domains option deletes every domain on this server before it puts the backup's back, and the legal holds (hm_discoveryholds) could not be read to learn whether any of them is held. Try again when the database answers (docs/Discovery.md). Nothing has been changed on this server.");

      return Formatter::Format("Restore refused: a restore with the domains option deletes every domain on this server before it puts the backup's back, and a legal hold stands on {0}. Release every hold on an account or a domain first, or restore the mailboxes you need one at a time with merge, which deletes nothing (docs/Discovery.md). Nothing has been changed on this server.", held);
   }
}
