// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "DomainSamples.h"
#include "Application.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../Util/Reports.h"
#include "../Util/Time.h"
#include "../Util/Parsing/StringParser.h"

#include <time.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const long long StoreSampleSeconds = 60 * 60;

      String DomainOf(const String &address)
      {
         String domain = StringParser::ExtractDomain(address);
         domain.ToLower();
         return domain;
      }
   }

   DomainSamples::DomainSamples() :
      store_sampled_at_(0)
   {
   }

   DomainSamples *
   DomainSamples::Instance()
   {
      static DomainSamples instance;
      return &instance;
   }

   String
   DomainSamples::Today()
   {
      return Time::GetCurrentDate();
   }

   void
   DomainSamples::RecordSpam(const String &domain)
   {
      if (domain.IsEmpty())
         return;

      boost::lock_guard<boost::mutex> guard(mutex_);
      pending_[domain].spam++;
   }

   void
   DomainSamples::RecordVirus(const String &domain)
   {
      if (domain.IsEmpty())
         return;

      boost::lock_guard<boost::mutex> guard(mutex_);
      pending_[domain].viruses++;
   }

   void
   DomainSamples::RecordFor(std::shared_ptr<MessageRecipients> recipients, bool spam)
   {
      if (!recipients)
         return;

      std::set<String> domains;

      // owned by this call: the recipients of the message its caller holds - a session's, a delivery's or a task's own
      for (std::shared_ptr<MessageRecipient> recipient : recipients->GetVector())
      {
         if (!recipient || !recipient->GetIsLocalName())
            continue;

         String domain = DomainOf(recipient->GetAddress());
         if (!domain.IsEmpty())
            domains.insert(domain);
      }

      for (const String &domain : domains)
      {
         if (spam)
            RecordSpam(domain);
         else
            RecordVirus(domain);
      }
   }

   bool
   DomainSamples::Upsert_(const String &day, const String &domain, int addSpam, int addViruses, __int64 storeBytes, __int64 storeMessages, bool setStore)
   {
      SQLCommand find("select dsid from hm_domainsamples where dsday = @DAY and dsdomain = @DOMAIN");
      find.AddParameter("@DAY", day);
      find.AddParameter("@DOMAIN", domain);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(find);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
      {
         SQLStatement insert;
         insert.SetStatementType(SQLStatement::STInsert);
         insert.SetTable("hm_domainsamples");
         insert.AddColumn("dsday", day);
         insert.AddColumn("dsdomain", domain);
         insert.AddColumn("dsspam", addSpam);
         insert.AddColumn("dsviruses", addViruses);
         insert.AddColumnInt64("dsstorebytes", setStore ? storeBytes : 0);
         insert.AddColumnInt64("dsstoremessages", setStore ? storeMessages : 0);

         return Application::Instance()->GetDBManager()->Execute(insert);
      }

      const __int64 id = recordset->GetInt64Value("dsid");

      AnsiString sql = "update hm_domainsamples set dsspam = dsspam + @SPAM, dsviruses = dsviruses + @VIRUSES";
      if (setStore)
         sql += ", dsstorebytes = @BYTES, dsstoremessages = @MESSAGES";
      sql += " where dsid = @ID";

      SQLCommand update(sql);
      update.AddParameter("@SPAM", addSpam);
      update.AddParameter("@VIRUSES", addViruses);
      if (setStore)
      {
         update.AddParameter("@BYTES", storeBytes);
         update.AddParameter("@MESSAGES", storeMessages);
      }
      update.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(update);
   }

   int
   DomainSamples::Flush()
   {
      std::map<String, Counts> taken;
      {
         boost::lock_guard<boost::mutex> guard(mutex_);
         taken.swap(pending_);
      }

      if (taken.empty())
         return 0;

      const String day = Today();
      int touched = 0;
      bool failed = false;

      for (std::map<String, Counts>::const_iterator it = taken.begin(); it != taken.end(); ++it)
      {
         if (Upsert_(day, it->first, it->second.spam, it->second.viruses, 0, 0, false))
            touched++;
         else
         {
            // Kept for the next flush rather than lost: a count that could
            // not be written is still a count.
            failed = true;
            boost::lock_guard<boost::mutex> guard(mutex_);
            pending_[it->first].spam += it->second.spam;
            pending_[it->first].viruses += it->second.viruses;
         }
      }

      return failed ? -1 : touched;
   }

   int
   DomainSamples::SampleStore(bool force)
   {
      const long long now = (long long) time(0);
      if (!force && store_sampled_at_ > 0 && now - store_sampled_at_ < StoreSampleSeconds && now >= store_sampled_at_)
         return 0;

      std::vector<Reports::MailboxRow> mailboxes;
      if (!Reports::ReadMailboxes(mailboxes))
         return 0;

      store_sampled_at_ = now;

      std::map<String, std::pair<__int64, __int64> > perDomain;
      for (const Reports::MailboxRow &row : mailboxes)
      {
         String domain = row.domain;
         domain.ToLower();
         if (domain.IsEmpty())
            continue;

         std::pair<__int64, __int64> &total = perDomain[domain];
         total.first += row.bytes;
         total.second += row.messages;
      }

      const String day = Today();
      int written = 0;

      for (std::map<String, std::pair<__int64, __int64> >::const_iterator it = perDomain.begin(); it != perDomain.end(); ++it)
      {
         if (Upsert_(day, it->first, 0, 0, it->second.first, it->second.second, true))
            written++;
      }

      return written;
   }

   bool
   DomainSamples::Prune(int days)
   {
      if (days <= 0)
         return false;

      time_t cutoff = time(0) - (time_t) days * 24 * 60 * 60;
      struct tm parts;
#ifdef _WIN32
      localtime_s(&parts, &cutoff);
#else
      localtime_r(&cutoff, &parts);
#endif
      String cutoffDay;
      cutoffDay.Format(_T("%d-%.02d-%.02d"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday);

      SQLCommand command("delete from hm_domainsamples where dsday < @DAY");
      command.AddParameter("@DAY", cutoffDay);

      return Application::Instance()->GetDBManager()->Execute(command);
   }
}
