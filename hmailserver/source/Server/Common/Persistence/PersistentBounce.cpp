// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentBounce.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../Util/Hashing/HashCreator.h"
#include "../Util/Unicode.h"
#include "../../SMTP/BounceClassifier.h"

#include <ctime>

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
      // knowledge AuditTrail applies to the statements it composes.
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

      // A search over at most this many rows of one domain. Searching is an
      // administrator reading a page, not the mail path, and the bound keeps a
      // pathological list from turning a keystroke into a table scan of millions.
      const int MaxSearchRows = 100000;

      // A sweep removes at most this many count rows per pass, so the hourly task
      // never holds the database for long; what is left goes in the next pass.
      const int MaxPrunePerPass = 5000;

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

      void ReadSuppression(std::shared_ptr<DALRecordset> recordset, BounceSuppressionEntry &entry)
      {
         entry.id = recordset->GetInt64Value("bsid");
         entry.domain_id = recordset->GetInt64Value("bsdomainid");
         entry.address = recordset->GetStringValue("bsaddress");
         entry.source = (int) recordset->GetLongValue("bssource");
         entry.category = (int) recordset->GetLongValue("bscategory");
         entry.status = recordset->GetStringValue("bsstatus");
         entry.diagnostic = recordset->GetStringValue("bsdiagnostic");
         entry.remote_mta = recordset->GetStringValue("bsremotemta");
         entry.created = recordset->GetStringValue("bscreated");
         entry.expires = recordset->GetStringValue("bsexpires");
      }

      void ReadCount(std::shared_ptr<DALRecordset> recordset, BounceCountRow &row)
      {
         row.id = recordset->GetInt64Value("bcid");
         row.domain_id = recordset->GetInt64Value("bcdomainid");
         row.address = recordset->GetStringValue("bcaddress");
         row.hard = (int) recordset->GetLongValue("bchard");
         row.soft = (int) recordset->GetLongValue("bcsoft");
         row.mailbox_full = (int) recordset->GetLongValue("bcmailboxfull");
         row.policy = (int) recordset->GetLongValue("bcpolicy");
         row.other = (int) recordset->GetLongValue("bcother");
         row.consecutive_hard = (int) recordset->GetLongValue("bcconsecutivehard");
         row.complaints = (int) recordset->GetLongValue("bccomplaints");
         row.last_status = recordset->GetStringValue("bclaststatus");
         row.last_category = (int) recordset->GetLongValue("bclastcategory");
         row.last_bounce = recordset->GetStringValue("bclastbounce");
         row.last_delivered = recordset->GetStringValue("bclastdelivered");
         row.last_complaint = recordset->GetStringValue("bclastcomplaint");
      }

      const wchar_t *SuppressionColumns = L"bsid, bsdomainid, bsaddress, bssource, bscategory, bsstatus, bsdiagnostic, bsremotemta, bscreated, bsexpires";
      const wchar_t *CountColumns = L"bcid, bcdomainid, bcaddress, bchard, bcsoft, bcmailboxfull, bcpolicy, bcother, bcconsecutivehard, bccomplaints, bclaststatus, bclastcategory, bclastbounce, bclastdelivered, bclastcomplaint";
   }

   const char *
   PersistentBounce::SourceName(int source)
   {
      switch (source)
      {
      case SourceBounce:    return "bounce";
      case SourceReport:    return "report";
      case SourceManual:    return "manual";
      case SourceComplaint: return "complaint";
      }

      return "bounce";
   }

   int
   PersistentBounce::SourceFromName(const String &name)
   {
      for (int source = SourceBounce; source <= SourceComplaint; source++)
      {
         if (name.CompareNoCase(String(SourceName(source)).c_str()) == 0)
            return source;
      }

      return -1;
   }

   String
   PersistentBounce::Canonical(const String &address)
   {
      String result = address;
      result.Trim();
      result.ToLower();
      return result.Left(255);
   }

   String
   PersistentBounce::DateTimeFromNow(int days)
   {
      std::time_t when = std::time(0) + (static_cast<std::time_t>(days) * 24 * 60 * 60);

      struct tm local;
      if (localtime_s(&local, &when) != 0)
         return String();

      char text[32];
      if (strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0)
         return String();

      return String(text);
   }

   int
   PersistentBounce::FindSuppression(__int64 domainID, const String &address, BounceSuppressionEntry &entry)
   {
      SQLCommand command(String(_T("select ")) + SuppressionColumns + _T(" from hm_bouncesuppressions where bsdomainid = @DOMAINID and bsaddress = @ADDRESS"));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESS", Canonical(address));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      if (recordset->IsEOF())
         return 0;

      ReadSuppression(recordset, entry);
      return 1;
   }

   bool
   PersistentBounce::InsertSuppression(const BounceSuppressionEntry &entry)
   {
      SQLStatement statement;
      statement.SetTable("hm_bouncesuppressions");
      statement.AddColumnInt64("bsdomainid", entry.domain_id);
      statement.AddColumn("bsaddress", Canonical(entry.address));
      statement.AddColumn("bssource", (long) entry.source);
      statement.AddColumn("bscategory", (long) entry.category);
      statement.AddColumn("bsstatus", entry.status.Left(16));
      statement.AddColumn("bsdiagnostic", entry.diagnostic.Left(255));
      statement.AddColumn("bsremotemta", entry.remote_mta.Left(255));
      statement.AddColumn("bscreated", entry.created.IsEmpty() ? DateTimeFromNow(0) : entry.created);
      statement.AddColumn("bsexpires", entry.expires);
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("bsid");

      return Db()->Execute(statement, 0, DALConnection::DALErrorInSQL);
   }

   int
   PersistentBounce::DeleteSuppression(__int64 domainID, const String &address)
   {
      SQLCommand parameters;
      parameters.AddParameter("@DOMAINID", domainID);
      parameters.AddParameter("@ADDRESS", Canonical(address));

      return DeleteWhere(_T("hm_bouncesuppressions"), _T("bsdomainid = @DOMAINID and bsaddress = @ADDRESS"), parameters);
   }

   bool
   PersistentBounce::ListSuppressions(__int64 domainID, const String &search, int limit, int offset,
                                      std::vector<BounceSuppressionEntry> &rows, int &total)
   {
      rows.clear();
      total = 0;

      if (limit < 1) limit = 1;
      if (limit > 1000) limit = 1000;
      if (offset < 0) offset = 0;

      String needle = search;
      needle.Trim();
      needle.ToLower();

      // A substring match in this process rather than a LIKE: the four databases
      // escape a LIKE pattern's own wildcards four different ways, and an address
      // is full of the one that matters (the underscore).
      if (!needle.IsEmpty())
      {
         SQLCommand command(SelectWithLimit(SuppressionColumns, _T("hm_bouncesuppressions"), _T("bsdomainid = @DOMAINID"), _T("bsid desc"), MaxSearchRows));
         command.AddParameter("@DOMAINID", domainID);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            BounceSuppressionEntry entry;
            ReadSuppression(recordset, entry);

            if (entry.address.Find(needle) >= 0)
            {
               if (total >= offset && (int) rows.size() < limit)
                  rows.push_back(entry);

               total++;
            }

            recordset->MoveNext();
         }

         return true;
      }

      SQLCommand countParameters;
      countParameters.AddParameter("@DOMAINID", domainID);
      total = CountWhere(_T("hm_bouncesuppressions"), _T("bsdomainid = @DOMAINID"), countParameters);
      if (total < 0)
      {
         total = 0;
         return false;
      }

      if (offset >= total)
         return true;

      SQLCommand command(SelectWithLimit(SuppressionColumns, _T("hm_bouncesuppressions"), _T("bsdomainid = @DOMAINID"), _T("bsid desc"), offset + limit));
      command.AddParameter("@DOMAINID", domainID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      int index = 0;
      while (!recordset->IsEOF() && (int) rows.size() < limit)
      {
         if (index >= offset)
         {
            BounceSuppressionEntry entry;
            ReadSuppression(recordset, entry);
            rows.push_back(entry);
         }

         index++;
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentBounce::IsExpired(const BounceSuppressionEntry &entry, int expiryDays, const String &now)
   {
      if (!entry.expires.IsEmpty() && entry.expires.Compare(now.c_str()) <= 0)
         return true;

      // Measured from when the entry was made, so lowering the setting applies to
      // the entries already there. A hand-made entry is kept until an administrator
      // says otherwise or its own expiry passes: somebody decided it deliberately.
      if (expiryDays > 0 && (entry.source == SourceBounce || entry.source == SourceReport))
      {
         const String cutoff = DateTimeFromNow(-expiryDays);

         if (!cutoff.IsEmpty() && entry.created.Compare(cutoff.c_str()) <= 0)
            return true;
      }

      return false;
   }

   int
   PersistentBounce::FindCount(__int64 domainID, const String &address, BounceCountRow &row)
   {
      SQLCommand command(String(_T("select ")) + CountColumns + _T(" from hm_bouncecounts where bcdomainid = @DOMAINID and bcaddress = @ADDRESS"));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESS", Canonical(address));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      if (recordset->IsEOF())
         return 0;

      ReadCount(recordset, row);
      return 1;
   }

   bool
   PersistentBounce::InsertCount(const BounceCountRow &row)
   {
      SQLStatement statement;
      statement.SetTable("hm_bouncecounts");
      statement.AddColumnInt64("bcdomainid", row.domain_id);
      statement.AddColumn("bcaddress", Canonical(row.address));
      statement.AddColumn("bchard", (long) row.hard);
      statement.AddColumn("bcsoft", (long) row.soft);
      statement.AddColumn("bcmailboxfull", (long) row.mailbox_full);
      statement.AddColumn("bcpolicy", (long) row.policy);
      statement.AddColumn("bcother", (long) row.other);
      statement.AddColumn("bcconsecutivehard", (long) row.consecutive_hard);
      statement.AddColumn("bccomplaints", (long) row.complaints);
      statement.AddColumn("bclaststatus", row.last_status.Left(16));
      statement.AddColumn("bclastcategory", (long) row.last_category);
      statement.AddColumn("bclastbounce", row.last_bounce);
      statement.AddColumn("bclastdelivered", row.last_delivered);
      statement.AddColumn("bclastcomplaint", row.last_complaint);
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("bcid");

      return Db()->Execute(statement, 0, DALConnection::DALErrorInSQL);
   }

   bool
   PersistentBounce::AddToCount(__int64 id, int category, int addToRun, const String &lastStatus, const String &lastBounce)
   {
      const wchar_t *counter = L"bcother";

      switch ((BounceCategory) category)
      {
      case BounceHard:        counter = L"bchard"; break;
      case BounceSoft:        counter = L"bcsoft"; break;
      case BounceMailboxFull: counter = L"bcmailboxfull"; break;
      case BouncePolicy:      counter = L"bcpolicy"; break;
      default:                counter = L"bcother"; break;
      }

      if (addToRun < 0)
         addToRun = 0;

      // The column name comes from the switch above and the run is an int, so
      // nothing in the statement's text came from outside.
      String sql;
      sql.Format(_T("update hm_bouncecounts set %s = %s + 1, bcconsecutivehard = bcconsecutivehard + %d, ")
                 _T("bclaststatus = @STATUS, bclastcategory = @CATEGORY, bclastbounce = @LASTBOUNCE where bcid = @ID"),
                 counter, counter, addToRun);

      SQLCommand command(sql);
      command.AddParameter("@STATUS", lastStatus.Left(16));
      command.AddParameter("@CATEGORY", category);
      command.AddParameter("@LASTBOUNCE", lastBounce);
      command.AddParameter("@ID", id);

      return Db()->Execute(command);
   }

   bool
   PersistentBounce::RecordDelivered(__int64 domainID, const String &address, const String &deliveredAt)
   {
      SQLCommand command(_T("update hm_bouncecounts set bcconsecutivehard = 0, bclastdelivered = @DELIVERED where bcdomainid = @DOMAINID and bcaddress = @ADDRESS"));
      command.AddParameter("@DELIVERED", deliveredAt.IsEmpty() ? DateTimeFromNow(0) : deliveredAt);
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESS", Canonical(address));

      return Db()->Execute(command);
   }

   bool
   PersistentBounce::AddComplaintToCount(__int64 id, const String &complainedAt)
   {
      // bclastcomplaint and not bclastdelivered: the two mean different things and
      // only one of them decides a later delivery report's run. See the header.
      SQLCommand command(_T("update hm_bouncecounts set bccomplaints = bccomplaints + 1, bclastcomplaint = @LASTCOMPLAINT where bcid = @ID"));
      command.AddParameter("@LASTCOMPLAINT", complainedAt.IsEmpty() ? DateTimeFromNow(0) : complainedAt);
      command.AddParameter("@ID", id);

      return Db()->Execute(command);
   }

   bool
   PersistentBounce::ResetConsecutiveHard(__int64 domainID, const String &address)
   {
      SQLCommand command(_T("update hm_bouncecounts set bcconsecutivehard = 0 where bcdomainid = @DOMAINID and bcaddress = @ADDRESS"));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESS", Canonical(address));

      return Db()->Execute(command);
   }

   bool
   PersistentBounce::ListCounts(__int64 domainID, const String &search, int limit, int offset,
                                std::vector<BounceCountRow> &rows, int &total)
   {
      rows.clear();
      total = 0;

      if (limit < 1) limit = 1;
      if (limit > 1000) limit = 1000;
      if (offset < 0) offset = 0;

      String needle = search;
      needle.Trim();
      needle.ToLower();

      if (!needle.IsEmpty())
      {
         SQLCommand command(SelectWithLimit(CountColumns, _T("hm_bouncecounts"), _T("bcdomainid = @DOMAINID"), _T("bcid desc"), MaxSearchRows));
         command.AddParameter("@DOMAINID", domainID);

         std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            BounceCountRow row;
            ReadCount(recordset, row);

            if (row.address.Find(needle) >= 0)
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
      total = CountWhere(_T("hm_bouncecounts"), _T("bcdomainid = @DOMAINID"), countParameters);
      if (total < 0)
      {
         total = 0;
         return false;
      }

      if (offset >= total)
         return true;

      SQLCommand command(SelectWithLimit(CountColumns, _T("hm_bouncecounts"), _T("bcdomainid = @DOMAINID"), _T("bcid desc"), offset + limit));
      command.AddParameter("@DOMAINID", domainID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return false;

      int index = 0;
      while (!recordset->IsEOF() && (int) rows.size() < limit)
      {
         if (index >= offset)
         {
            BounceCountRow row;
            ReadCount(recordset, row);
            rows.push_back(row);
         }

         index++;
         recordset->MoveNext();
      }

      return true;
   }

   String
   PersistentBounce::AddressKey(const String &address)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(Canonical(address), utf8);

      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(utf8, HashCreator::hex));
   }

   bool
   PersistentBounce::InsertReportKey(__int64 domainID, const String &key, const String &addressKey, int run, const String &sent)
   {
      SQLStatement statement;
      statement.SetTable("hm_bouncereportkeys");
      statement.AddColumnInt64("brkdomainid", domainID);
      statement.AddColumn("brkkey", key.Left(64));
      statement.AddColumn("brksent", sent.IsEmpty() ? DateTimeFromNow(0) : sent);
      statement.AddColumn("brkaddress", addressKey.Left(64));
      statement.AddColumn("brkrun", (long) (run < 0 ? 0 : run));
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("brkid");

      return Db()->Execute(statement);
   }

   int
   PersistentBounce::ConsumeReportKey(const String &key, BounceReportKeyRow &row)
   {
      row = BounceReportKeyRow();

      // Two statements, and deliberately not "delete ... where brkid = (select
      // min(brkid) ...)": SQL Server Compact refuses a scalar subquery compared
      // with =, and that exact shape has voided a regression gate before.
      SQLCommand command(SelectWithLimit(_T("brkid, brkdomainid, brksent, brkaddress, brkrun"), _T("hm_bouncereportkeys"), _T("brkkey = @KEY"), _T("brkid"), 1));
      command.AddParameter("@KEY", key.Left(64));

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      if (recordset->IsEOF())
         return 0;

      BounceReportKeyRow found;
      found.id = recordset->GetInt64Value("brkid");
      found.domain_id = recordset->GetInt64Value("brkdomainid");
      found.sent = recordset->GetStringValue("brksent");
      found.address_key = recordset->GetStringValue("brkaddress");
      found.run = (int) recordset->GetLongValue("brkrun");

      SQLCommand remove(_T("delete from hm_bouncereportkeys where brkid = @ID"));
      remove.AddParameter("@ID", found.id);

      if (!Db()->Execute(remove))
         return -1;

      row = found;
      return 1;
   }

   int
   PersistentBounce::FindLaterReportKey(__int64 domainID, const String &addressKey, __int64 afterID, __int64 &laterID)
   {
      laterID = 0;

      SQLCommand command(SelectWithLimit(_T("brkid"), _T("hm_bouncereportkeys"),
         _T("brkdomainid = @DOMAINID and brkaddress = @ADDRESSKEY and brkid > @AFTERID"), _T("brkid"), 1));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESSKEY", addressKey.Left(64));
      command.AddParameter("@AFTERID", afterID);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      if (recordset->IsEOF())
         return 0;

      laterID = recordset->GetInt64Value("brkid");
      return 1;
   }

   bool
   PersistentBounce::AddToReportKeyRun(__int64 id, int amount)
   {
      if (amount <= 0)
         return true;

      String sql;
      sql.Format(_T("update hm_bouncereportkeys set brkrun = brkrun + %d where brkid = @ID"), amount);

      SQLCommand command(sql);
      command.AddParameter("@ID", id);

      return Db()->Execute(command);
   }

   bool
   PersistentBounce::ClearReportKeyRuns(__int64 domainID, const String &addressKey)
   {
      SQLCommand command(_T("update hm_bouncereportkeys set brkrun = 0 where brkdomainid = @DOMAINID and brkaddress = @ADDRESSKEY"));
      command.AddParameter("@DOMAINID", domainID);
      command.AddParameter("@ADDRESSKEY", addressKey.Left(64));

      return Db()->Execute(command);
   }

   int
   PersistentBounce::ExpireSuppressions(int expiryDays)
   {
      int removed = 0;

      SQLCommand explicitExpiry;
      explicitExpiry.AddParameter("@NOW", DateTimeFromNow(0));

      int count = DeleteWhere(_T("hm_bouncesuppressions"), _T("bsexpires <> '' and bsexpires <= @NOW"), explicitExpiry);
      if (count < 0)
         return -1;

      removed += count;

      if (expiryDays > 0)
      {
         SQLCommand aged;
         aged.AddParameter("@CUTOFF", DateTimeFromNow(-expiryDays));

         count = DeleteWhere(_T("hm_bouncesuppressions"),
            _T("bssource <= 1 and bscreated <= @CUTOFF"), aged);

         if (count < 0)
            return -1;

         removed += count;
      }

      return removed;
   }

   int
   PersistentBounce::PruneCounts(int retentionDays)
   {
      if (retentionDays <= 0)
         return 0;

      const String cutoff = DateTimeFromNow(-retentionDays);

      // Candidates in one select, then each checked against the list and deleted by
      // id. A count behind an entry that is still on the list is kept whatever its
      // age: it is the entry's history, and the page shows the two side by side.
      // Three names for the one value, in the order they appear: SQL Server and
      // Compact bind parameters by position, and a name used three times would be
      // three placeholders with one value behind them.
      //
      // All three dates have to be old, and an empty one counts as old on every
      // backend because '' sorts before any timestamp. That is what keeps a row a
      // complaint made - which has no bounce and no delivery - alive for the
      // retention period after the LAST complaint rather than the first: without
      // bclastcomplaint in this predicate such a row has no date at all, is swept
      // on the next hourly pass, and an address complained about once a quarter
      // never reaches a threshold above one.
      SQLCommand command(SelectWithLimit(_T("bcid, bcdomainid, bcaddress"), _T("hm_bouncecounts"),
         _T("bclastbounce <= @BOUNCECUTOFF and bclastdelivered <= @DELIVEREDCUTOFF and bclastcomplaint <= @COMPLAINTCUTOFF"), _T("bcid"), MaxPrunePerPass));
      command.AddParameter("@BOUNCECUTOFF", cutoff);
      command.AddParameter("@DELIVEREDCUTOFF", cutoff);
      command.AddParameter("@COMPLAINTCUTOFF", cutoff);

      std::shared_ptr<DALRecordset> recordset = Db()->OpenRecordset(command);
      if (!recordset)
         return -1;

      std::vector<std::pair<__int64, std::pair<__int64, String> > > candidates;
      while (!recordset->IsEOF())
      {
         candidates.push_back(std::make_pair(recordset->GetInt64Value("bcid"),
            std::make_pair(recordset->GetInt64Value("bcdomainid"), recordset->GetStringValue("bcaddress"))));
         recordset->MoveNext();
      }

      int removed = 0;

      for (const auto &candidate : candidates)
      {
         BounceSuppressionEntry entry;
         const int listed = FindSuppression(candidate.second.first, candidate.second.second, entry);

         if (listed != 0)
            continue;

         SQLCommand remove(_T("delete from hm_bouncecounts where bcid = @ID"));
         remove.AddParameter("@ID", candidate.first);

         if (!Db()->Execute(remove))
            return -1;

         removed++;
      }

      return removed;
   }

   int
   PersistentBounce::PruneReportKeys(int windowDays)
   {
      if (windowDays < 1)
         windowDays = 1;

      SQLCommand parameters;
      parameters.AddParameter("@CUTOFF", DateTimeFromNow(-windowDays));

      return DeleteWhere(_T("hm_bouncereportkeys"), _T("brksent <= @CUTOFF"), parameters);
   }

   int
   PersistentBounce::EraseAddress(const String &address)
   {
      SQLCommand parameters;
      parameters.AddParameter("@ADDRESS", Canonical(address));

      const int counts = DeleteWhere(_T("hm_bouncecounts"), _T("bcaddress = @ADDRESS"), parameters);
      if (counts < 0)
         return -1;

      const int entries = DeleteWhere(_T("hm_bouncesuppressions"), _T("bsaddress = @ADDRESS"), parameters);
      if (entries < 0)
         return -1;

      // The register names the address only by its hash, and that is enough to
      // find what it holds about it.
      SQLCommand keyParameters;
      keyParameters.AddParameter("@ADDRESSKEY", AddressKey(address).Left(64));

      const int keys = DeleteWhere(_T("hm_bouncereportkeys"), _T("brkaddress = @ADDRESSKEY"), keyParameters);
      if (keys < 0)
         return -1;

      return counts + entries + keys;
   }
}
