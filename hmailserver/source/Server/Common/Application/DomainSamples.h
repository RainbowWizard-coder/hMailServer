// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <map>
#include <boost/thread/mutex.hpp>

namespace HM
{
   class MessageRecipients;

   // The per-domain daily sample: what the server-wide counters could not say.
   // Spam detected and viruses removed are counted here by the domain of each
   // local recipient as they happen, kept in memory and added to today's row
   // of hm_domainsamples (one per day and domain) when the metric sampler runs;
   // the store's size per domain is one grouped pass over the message store,
   // at most once an hour, written into the same row. The reports read the
   // rows; the retention is the metric history's.
   class DomainSamples
   {
   public:
      static DomainSamples *Instance();

      // One spam message, or one removed virus, for every local recipient
      // domain the message had - a message to two people in one domain counts
      // once for it. No recipient known (a refusal at MAIL FROM) counts nowhere.
      void RecordFor(std::shared_ptr<MessageRecipients> recipients, bool spam);
      void RecordSpam(const String &domain);
      void RecordVirus(const String &domain);

      // Adds what has been counted since the last flush to today's rows.
      // Returns the rows touched; -1 when a write failed (the counts are kept
      // for the next flush).
      int Flush();

      // Writes every local domain's store size into today's row, at most once
      // an hour unless force. Returns the rows written, or 0 when it was not
      // time yet.
      int SampleStore(bool force);

      // Removes rows older than days. Returns whether the delete ran.
      static bool Prune(int days);

      // "YYYY-MM-DD", the server's local day.
      static String Today();

   private:
      DomainSamples();

      struct Counts
      {
         Counts() : spam(0), viruses(0) { }
         int spam;
         int viruses;
      };

      static bool Upsert_(const String &day, const String &domain, int addSpam, int addViruses, __int64 storeBytes, __int64 storeMessages, bool setStore);

      boost::mutex mutex_;
      std::map<String, Counts> pending_;
      long long store_sampled_at_;
   };
}
