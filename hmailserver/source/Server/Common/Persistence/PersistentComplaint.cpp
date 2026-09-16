// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentComplaint.h"

#include "PersistentBounce.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <ctime>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A row-limited SELECT for each backend: TOP (n) on SQL Server and Compact,
      // which requires the parentheses, LIMIT n on MySQL and PostgreSQL. The same
      // knowledge PersistentBounce and AuditTrail apply to their statements.
      String SelectWithLimit(const String &columns, const String &from, const String &where, const String &orderBy, int rows)
      {
         String statement;

         switch (IniFileSettings::Instance()->GetDatabaseType())
         {
         case DatabaseSettings::TypeMSSQLServer:
         case DatabaseSettings::TypeMSSQLCompactEdition:
            statement.Format(_T("select top (%d) %s from %s"), rows, columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            break;
         default:
            statement.Format(_T("select %s from %s"), columns.c_str(), from.c_str());
            if (!where.IsEmpty())
               statement += _T(" where ") + where;
            if (!orderBy.IsEmpty())
               statement += _T(" order by ") + orderBy;
            statement += Formatter::Format(_T(" limit {0}"), rows);
            break;
         }

         return statement;
      }

      DatabaseConnectionManager *Db()
      {
         return Application::Instance()->GetDBManager().get();
      }

      // A search reads at most this many of one domain's complaints. Searching is an
      // administrator reading a page, not the mail path.
      const int MaxSearchRows = 100000;

      // A sweep removes at most this many rows per pass, so the hourly task never
      // holds the database for long; what is left goes in the next pass.
      const int MaxPrunePerPass = 5000;

      // The tally is one row per domain per day, so a window is small; this is the
      // ceiling that keeps a misconfigured retention from reading a huge table.
      const int MaxVolumeRows = 100000;

      const wchar_t *ComplaintColumns =
         L"cpid, cpdomainid, cpaccountid, cpkey, cpaddress, cpsender, cpfeedbacktype, cpprovider, "
         L"cpsourceip, cpreporteddomain, cpmessageid, cpsubject, cparrival, cpreceived";

      void ReadComplaint(std::shared_ptr<DALRecordset> recordset, ComplaintRow &row)
      {
         row.id = recordset->GetInt64Value("cpid");
         row.domain_id = recordset->GetInt64Value("cpdomainid");
         row.account_id = recordset->GetInt64Value("cpaccountid");
         row.key = recordset->GetStringValue("cpkey");
         row.address = recordset->GetStringValue("cpaddress");
         row.sender = recordset->GetStringValue("cpsender");
         row.feedback_type = recordset->GetStringValue("cpfeedbacktype");
         row.provider = recordset->GetStringValue("cpprovider");
         row.source_ip = recordset->GetStringValue("cpsourceip");
         row.reported_domain = recordset->GetStringValue("cpreporteddomain");
         row.message_id = recordset->GetStringValue("cpmessageid");
         row.subject = recordset->GetStringValue("cpsubject");
         row.arrival = recordset->GetStringValue("cparrival");
         row.received = recordset->GetStringValue("cpreceived");
      }

      // parameters carries the parameters and nothing else; its query text is
      // replaced here, once for the count and once for the delete.
      int CountWhere(const String &table, const String &where, const SQLCommand &parameters)
      {
         SQLCommand command = parameters;
         command.SetQueryString(_T("select count(*) as c from ") + table + _T(" where ") + where);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return -1;

         if (recordset->IsEOF())
            return 0;

         return (int) recordset->GetLongValue("c");
      }

      int DeleteWhere(const String &table, const String &where, const SQLCommand &parameters)
      {
         const int count = CountWhere(table, where, parameters);
         if (count <= 0)
            return count;

         SQLCommand command = parameters;
         command.SetQueryString(_T("delete from ") + table + _T(" where ") + where);

         if (!Db()->Execute(command))
            return -1;

         return count;
      }
   }

   String
   PersistentComplaint::Day(int daysFromNow)
   {
      std::time_t when = std::time(0) + (static_cast<std::time_t>(daysFromNow) * 24 * 60 * 60);

      struct tm local;
      if (localtime_s(&local, &when) != 0)
         return String();

      char text[16];
      if (strftime(text, sizeof(text), "%Y-%m-%d", &local) == 0)
         return String();

      return String(text);
   }

   int
   PersistentComplaint::Insert(const ComplaintRow &row)
   {
      SQLStatement statement;
      statement.SetTable("hm_complaints");
      statement.AddColumnInt64("cpdomainid", row.domain_id);
      statement.AddColumnInt64("cpaccountid", row.account_id);
      statement.AddColumn("cpkey", row.key.Left(64));
      statement.AddColumn("cpaddress", PersistentBounce::Canonical(row.address));
      statement.AddColumn("cpsender", PersistentBounce::Canonical(row.sender));
      statement.AddColumn("cpfeedbacktype", row.feedback_type.Left(32));
      statement.AddColumn("cpprovider", row.provider.Left(255));
      statement.AddColumn("cpsourceip", row.source_ip.Left(45));
      statement.AddColumn("cpreporteddomain", row.reported_domain.Left(255));
      statement.AddColumn("cpmessageid", row.message_id.Left(255));
      statement.AddColumn("cpsubject", row.subject.Left(255));
      statement.AddColumn("cparrival", row.arrival.Left(32));
      statement.AddColumn("cpreceived", row.received.IsEmpty() ? PersistentBounce::DateTimeFromNow(0) : row.received.Left(32));
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("cpid");

      // The unique index on the key decides a race between two nodes reading the
      // same report at once, and recognises a provider that sends the same report
      // twice. The loser is not an error, so none is written: the insert is read
      // back, and a row with this key means the complaint is already recorded.
      if (Db()->Execute(statement, 0, DALConnection::DALErrorInSQL))
         return 1;

      SQLCommand command(_T("select cpid from hm_complaints where cpkey = @KEY"));
      command.AddParameter("@KEY", row.key.Left(64));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      return recordset->IsEOF() ? -1 : 0;
   }

   bool
   PersistentComplaint::List(__int64 domainID, __int64 accountID, const String &search, int limit, int offset,
                             std::vector<ComplaintRow> &rows, int &total)
   {
      rows.clear();
      total = 0;

      if (limit < 1) limit = 1;
      if (limit > 1000) limit = 1000;
      if (offset < 0) offset = 0;

      String needle = search;
      needle.Trim();
      needle.ToLower();

      const bool filtered = !needle.IsEmpty() || accountID > 0;

      // A substring match in this process rather than a LIKE: the four databases
      // escape a LIKE pattern's own wildcards four different ways, and an address is
      // full of the one that matters (the underscore). The account filter is applied
      // here too, so one shape reads both.
      if (filtered)
      {
         SQLCommand command(SelectWithLimit(ComplaintColumns, _T("hm_complaints"), _T("cpdomainid = @DOMAINID"), _T("cpid desc"), MaxSearchRows));
         command.AddParameter("@DOMAINID", domainID);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            ComplaintRow row;
            ReadComplaint(recordset, row);

            String haystack = row.address + _T(" ") + row.sender + _T(" ") + row.subject + _T(" ") + row.provider;
            haystack.ToLower();

            const bool matches = (needle.IsEmpty() || haystack.Find(needle) >= 0) &&
                                 (accountID <= 0 || row.account_id == accountID);

            if (matches)
            {
               if (total >= offset && (int) rows.size() < limit)
                  rows.push_back(row);

               total++;
            }

            recordset->MoveNext();
         }

         return true;
      }

      SQLCommand countParameters;
      countParameters.AddParameter("@DOMAINID", domainID);
      total = CountWhere(_T("hm_complaints"), _T("cpdomainid = @DOMAINID"), countParameters);
      if (total < 0)
      {
         total = 0;
         return false;
      }

      if (offset >= total)
         return true;

      SQLCommand command(SelectWithLimit(ComplaintColumns, _T("hm_complaints"), _T("cpdomainid = @DOMAINID"), _T("cpid desc"), offset + limit));
      command.AddParameter("@DOMAINID", domainID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      int index = 0;
      while (!recordset->IsEOF() && (int) rows.size() < limit)
      {
         if (index >= offset)
         {
            ComplaintRow row;
            ReadComplaint(recordset, row);
            rows.push_back(row);
         }

         index++;
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentComplaint::AddToVolume(__int64 domainID, const String &day, __int64 sent, int complaints)
   {
      if (domainID <= 0 || day.IsEmpty())
         return true;

      if (sent < 0) sent = 0;
      if (complaints < 0) complaints = 0;
      if (sent == 0 && complaints == 0)
         return true;

      // Twice at most: the second time is the insert that lost a race with another
      // node or another delivery thread, read back and applied to the row it wrote.
      for (int attempt = 0; attempt < 2; attempt++)
      {
         SQLCommand find(_T("select cvid from hm_complaintvolume where cvdomainid = @DOMAINID and cvday = @DAY"));
         find.AddParameter("@DOMAINID", domainID);
         find.AddParameter("@DAY", day.Left(10));

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(find);
         if (!recordset)
            return false;

         if (recordset->IsEOF())
         {
            SQLStatement statement;
            statement.SetTable("hm_complaintvolume");
            statement.AddColumnInt64("cvdomainid", domainID);
            statement.AddColumn("cvday", day.Left(10));
            statement.AddColumnInt64("cvsent", sent);
            statement.AddColumn("cvcomplaints", (long) complaints);
            statement.SetStatementType(SQLStatement::STInsert);
            statement.SetIdentityColumn("cvid");

            if (Db()->Execute(statement, 0, DALConnection::DALErrorInSQL))
               return true;

            // A lost insert is a race: read the row back and add to it.
            continue;
         }

         const __int64 id = recordset->GetInt64Value("cvid");

         // Both amounts are numbers this server computed - recipients delivered in
         // one pass, and one complaint - so nothing in the statement's text came
         // from outside. They are literals because SQL Server Compact cannot type a
         // parameter used in arithmetic.
         String sql;
         sql.Format(_T("update hm_complaintvolume set cvsent = cvsent + %d, cvcomplaints = cvcomplaints + %d where cvid = @ID"),
                    (int) sent, complaints);

         SQLCommand command(sql);
         command.AddParameter("@ID", id);

         return Db()->Execute(command);
      }

      return false;
   }

   bool
   PersistentComplaint::ReadVolume(const String &since, std::vector<ComplaintVolumeRow> &rows)
   {
      rows.clear();

      if (since.IsEmpty() || Db() == nullptr)
         return false;

      // The days are added up here rather than by a GROUP BY: one row per domain per
      // day over a window of days is a handful of rows, and the four databases agree
      // on nothing about aggregates that this does not have to know.
      SQLCommand command(SelectWithLimit(_T("cvdomainid, cvsent, cvcomplaints"), _T("hm_complaintvolume"),
         _T("cvday >= @SINCE"), _T("cvid"), MaxVolumeRows));
      command.AddParameter("@SINCE", since.Left(10));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      std::map<__int64, ComplaintVolumeRow> totals;

      while (!recordset->IsEOF())
      {
         const __int64 domainID = recordset->GetInt64Value("cvdomainid");

         ComplaintVolumeRow &row = totals[domainID];
         row.domain_id = domainID;
         row.sent += recordset->GetInt64Value("cvsent");
         row.complaints += recordset->GetInt64Value("cvcomplaints");

         recordset->MoveNext();
      }

      for (const auto &entry : totals)
         rows.push_back(entry.second);

      return true;
   }

   int
   PersistentComplaint::Prune(int retentionDays, int tallyRetentionDays)
   {
      int removed = 0;

      if (retentionDays <= 0 && tallyRetentionDays <= 0)
         return 0;

      // The complaints, by when this server read them. Candidates in one select and
      // then deleted by id, so a pass is bounded whatever the table holds.
      if (retentionDays > 0)
      {
         SQLCommand candidates(SelectWithLimit(_T("cpid"), _T("hm_complaints"), _T("cpreceived <= @CUTOFF"), _T("cpid"), MaxPrunePerPass));
         candidates.AddParameter("@CUTOFF", PersistentBounce::DateTimeFromNow(-retentionDays));

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(candidates);
         if (!recordset)
            return -1;

         std::vector<__int64> ids;
         while (!recordset->IsEOF())
         {
            ids.push_back(recordset->GetInt64Value("cpid"));
            recordset->MoveNext();
         }

         for (__int64 id : ids)
         {
            SQLCommand remove(_T("delete from hm_complaints where cpid = @ID"));
            remove.AddParameter("@ID", id);

            if (!Db()->Execute(remove))
               return -1;

            removed++;
         }
      }

      // The tally, by the day it is for, on its OWN clock. The caller passes the
      // larger of the retention and the rate window, so a day that is still inside
      // a window this server can be asked for is never deleted - which the earlier
      // shape of this function asserted ("the window is shorter than it") without
      // anything enforcing it: a retention of three days would have thrown away
      // four of the seven days the rate is worked out from, and the rate would have
      // read low with nothing to say so.
      if (tallyRetentionDays <= 0)
         return removed;

      SQLCommand days;
      days.AddParameter("@DAYCUTOFF", Day(-tallyRetentionDays));

      const int tally = DeleteWhere(_T("hm_complaintvolume"), _T("cvday <= @DAYCUTOFF"), days);
      if (tally < 0)
         return -1;

      return removed + tally;
   }

   int
   PersistentComplaint::EraseAddress(const String &address)
   {
      // Two statements rather than one with an OR on two names: SQL Server and
      // Compact bind parameters by position, so a name used twice would be two
      // placeholders with one value behind them.
      SQLCommand asReader;
      asReader.AddParameter("@ADDRESS", PersistentBounce::Canonical(address));

      const int complainedAbout = DeleteWhere(_T("hm_complaints"), _T("cpaddress = @ADDRESS"), asReader);
      if (complainedAbout < 0)
         return -1;

      SQLCommand asSender;
      asSender.AddParameter("@SENDER", PersistentBounce::Canonical(address));

      const int sent = DeleteWhere(_T("hm_complaints"), _T("cpsender = @SENDER"), asSender);
      if (sent < 0)
         return -1;

      return complainedAbout + sent;
   }
}
