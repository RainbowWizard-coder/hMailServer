// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SenderListCache.h"
#include "SenderPattern.h"
#include "SURBL.h"
#include "../BO/SenderListEntry.h"
#include "../BO/SenderListEntries.h"

#include <set>

#include <boost/thread/shared_mutex.hpp>
#include <boost/thread/locks.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      boost::shared_mutex sender_list_mutex;
   }

   std::vector<std::shared_ptr<SenderListEntry> > SenderListCache::entries_;
   bool SenderListCache::need_refresh_ = true;
   unsigned long long SenderListCache::generation_ = 0;

   void
   SenderListCache::SetNeedRefresh()
   {
      need_refresh_ = true;
   }

   void
   SenderListCache::Refresh_()
   {
      SenderListEntries all;

      if (!all.RefreshAll())
      {
         // The database could not be read. Keep what is here and leave the flag
         // set so the next message retries. Replacing the list with an empty one
         // would silently drop every allow entry - and an allow entry is the
         // thing standing between a customer's invoice and the junk folder, so
         // "we could not read it, therefore nobody allowed anything" is the one
         // answer that must never be given.
         return;
      }

      // owned by this call: all is the local collection just loaded; the cache's copy is guarded by the cache's own lock
      entries_ = all.GetConstVector();
      need_refresh_ = false;

      // A message's links read against the entries before this are read
      // again before they decide anything (IsCurrent).
      generation_++;
   }

   bool
   SenderListCache::IsEmpty()
   {
      boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

      if (need_refresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      return entries_.empty();
   }

   String
   SenderListCache::ScopeName(int scope)
   {
      switch (scope)
      {
      case SenderListEntry::ScopeAccount:
         return _T("account");
      case SenderListEntry::ScopeDomain:
         return _T("domain");
      case SenderListEntry::ScopeServer:
         return _T("server");
      default:
         return _T("unknown");
      }
   }

   SenderListCache::Decision
   SenderListCache::Evaluate_(const String &envelopeSender, const String &headerFrom, int scope, __int64 ownerId)
   {
      Decision decision;
      decision.scope = scope;

      int bestSpecificity = -1;

      for (const std::shared_ptr<SenderListEntry> &entry : entries_)
      {
         if (entry->GetScope() != scope)
            continue;

         if (entry->GetOwnerID() != ownerId)
            continue;

         if (entry->HasExpired())
            continue;

         // The envelope first, so that when an entry covers both addresses the
         // log names the one the SMTP conversation carried.
         String matched;

         if (entry->Matches(envelopeSender))
            matched = envelopeSender;
         else if (!headerFrom.IsEmpty() && entry->Matches(headerFrom))
            matched = headerFrom;
         else
            continue;

         const int specificity = entry->Specificity();
         const Verdict verdict = entry->GetBlocked() ? Blocked : Allowed;

         if (specificity > bestSpecificity)
         {
            bestSpecificity = specificity;
            decision.verdict = verdict;
            decision.value = entry->GetValue();
            decision.reason = entry->GetReason();
            decision.matched = matched;
            continue;
         }

         // An exact tie between an allow and a block. Block wins; see the header
         // for why. The allow's text is discarded so the log line and the header
         // name the entry that actually decided.
         if (specificity == bestSpecificity && verdict == Blocked && decision.verdict != Blocked)
         {
            decision.verdict = Blocked;
            decision.value = entry->GetValue();
            decision.reason = entry->GetReason();
            decision.matched = matched;
         }
      }

      return decision;
   }

   SenderListCache::Decision
   SenderListCache::Decide(const String &envelopeSender, const String &headerFrom, __int64 accountId, __int64 domainId)
   {
      Decision decision;

      if (envelopeSender.IsEmpty())
      {
         // The null sender carries bounces. Nothing lists it, and nothing may
         // stop it, whatever its From header claims: a list that could swallow a
         // delivery status notification would make a delivery failure invisible
         // to the person who sent the message.
         return decision;
      }

      boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

      if (need_refresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      if (entries_.empty())
         return decision;

      // Nearest first, and the first scope with anything to say is the last one
      // consulted. Written as a loop over the three scopes in order rather than
      // as three blocks, so that the order is one line to read and one line to
      // change.
      const int scopes[] =
      {
         SenderListEntry::ScopeAccount,
         SenderListEntry::ScopeDomain,
         SenderListEntry::ScopeServer
      };

      for (const int scope : scopes)
      {
         __int64 ownerId = 0;

         if (scope == SenderListEntry::ScopeAccount)
         {
            if (accountId == 0)
               continue;
            ownerId = accountId;
         }
         else if (scope == SenderListEntry::ScopeDomain)
         {
            if (domainId == 0)
               continue;
            ownerId = domainId;
         }

         decision = Evaluate_(envelopeSender, headerFrom, scope, ownerId);

         if (decision.verdict != NoOpinion)
            return decision;
      }

      Decision none;
      return none;
   }

   bool
   SenderListCache::OwnerAtScope_(int scope, __int64 accountId, __int64 domainId, __int64 &ownerId)
   {
      ownerId = 0;

      if (scope == SenderListEntry::ScopeAccount)
      {
         if (accountId == 0)
            return false;
         ownerId = accountId;
      }
      else if (scope == SenderListEntry::ScopeDomain)
      {
         if (domainId == 0)
            return false;
         ownerId = domainId;
      }

      return true;
   }

   void
   SenderListCache::ReadUrlLinks(std::shared_ptr<MessageData> messageData, UrlLinks &links)
   {
      // The hosts the entries name, taken under the lock; the message is read
      // without it.
      std::set<String> entryHosts;
      {
         boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

         if (need_refresh_)
         {
            boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
            Refresh_();
         }

         links.generation = generation_;
         for (const std::shared_ptr<SenderListEntry> &entry : entries_)
         {
            if (entry->IsUrl() && !entry->HasExpired())
               entryHosts.insert(entry->UrlHost());
         }
      }

      links.covered.clear();
      if (entryHosts.empty())
         return;

      // Until 6.3.6 the first 16 hosts of a message were
      // matched, and an entry's link after sixteen others was never seen:
      // every one is now, each looked up by the names an entry for it could
      // have (SenderPattern::UrlEntryHostsCovering).
      SURBL::ForEachLinkHost(messageData, [&](const String &host) -> bool
      {
         CoverLink(entryHosts, host, links.covered);
         return true;
      });
   }

   void
   SenderListCache::CoverLink(const std::set<String> &entryHosts, const String &linkHost, std::map<String, String> &covered)
   {
      std::vector<String> covering;
      SenderPattern::UrlEntryHostsCovering(linkHost, covering);

      // The first link an entry host covers is the one the log and the reason
      // header name: insert leaves an entry host already recorded as it is.
      for (const String &entryHost : covering)
      {
         if (entryHosts.find(entryHost) != entryHosts.end())
            covered.insert(std::make_pair(entryHost, linkHost));
      }
   }

   bool
   SenderListCache::IsCurrent(const UrlLinks &links)
   {
      boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

      if (need_refresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      return links.generation == generation_;
   }

   SenderListCache::Decision
   SenderListCache::EvaluateUrls_(const UrlLinks &links, int scope, __int64 ownerId)
   {
      Decision decision;
      decision.scope = scope;

      int bestSpecificity = -1;

      for (const std::shared_ptr<SenderListEntry> &entry : entries_)
      {
         if (entry->GetScope() != scope || entry->GetOwnerID() != ownerId || entry->HasExpired() || !entry->IsUrl())
            continue;

         const auto covered = links.covered.find(entry->UrlHost());
         if (covered == links.covered.end())
            continue;

         const String matched = covered->second;

         const int specificity = entry->Specificity();
         const Verdict verdict = entry->GetBlocked() ? Blocked : Allowed;

         if (specificity > bestSpecificity)
         {
            bestSpecificity = specificity;
            decision.verdict = verdict;
            decision.value = entry->GetValue();
            decision.reason = entry->GetReason();
            decision.matched = matched;
            continue;
         }

         // The same tie-break as for senders: on equal specificity, block wins.
         if (specificity == bestSpecificity && verdict == Blocked && decision.verdict != Blocked)
         {
            decision.verdict = Blocked;
            decision.value = entry->GetValue();
            decision.reason = entry->GetReason();
            decision.matched = matched;
         }
      }

      return decision;
   }

   bool
   SenderListCache::HasUrlEntries(__int64 accountId, __int64 domainId)
   {
      boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

      if (need_refresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      for (const std::shared_ptr<SenderListEntry> &entry : entries_)
      {
         if (!entry->IsUrl() || entry->HasExpired())
            continue;

         __int64 ownerId = 0;
         if (!OwnerAtScope_(entry->GetScope(), accountId, domainId, ownerId))
            continue;

         if (entry->GetOwnerID() == ownerId)
            return true;
      }

      return false;
   }

   SenderListCache::Decision
   SenderListCache::DecideUrls(const UrlLinks &links, __int64 accountId, __int64 domainId)
   {
      Decision decision;

      if (links.covered.empty())
         return decision;

      boost::upgrade_lock<boost::shared_mutex> lock(sender_list_mutex);

      if (need_refresh_)
      {
         boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
         Refresh_();
      }

      if (entries_.empty())
         return decision;

      const int scopes[] =
      {
         SenderListEntry::ScopeAccount,
         SenderListEntry::ScopeDomain,
         SenderListEntry::ScopeServer
      };

      for (const int scope : scopes)
      {
         __int64 ownerId = 0;
         if (!OwnerAtScope_(scope, accountId, domainId, ownerId))
            continue;

         decision = EvaluateUrls_(links, scope, ownerId);

         if (decision.verdict != NoOpinion)
            return decision;
      }

      Decision none;
      return none;
   }
}
