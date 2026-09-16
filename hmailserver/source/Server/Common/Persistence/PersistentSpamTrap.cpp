// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentSpamTrap.h"
#include "../BO/SpamTrap.h"
#include "../AntiSpam/SpamTrapMonitor.h"
#include "../Application/IniFileSettings.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseSettings.h"
#include "../Util/Time.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The column widths of the create scripts. A value longer than its column is
      // cut rather than refused: this is a record of something that already
      // happened, and a hit that could not be written because a spammer sent a
      // long envelope sender is a hit nobody learns about.
      String Fit(const String &value, int width)
      {
         return value.GetLength() > width ? value.Left(width) : value;
      }
   }

   PersistentSpamTrap::PersistentSpamTrap()
   {
   }

   PersistentSpamTrap::~PersistentSpamTrap()
   {
   }

   bool
   PersistentSpamTrap::DeleteObject(std::shared_ptr<SpamTrap> pObject)
   {
      SQLCommand command("delete from hm_spamtraps where stid = @STID");
      command.AddParameter("@STID", pObject->GetID());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      // On the delete as well as on the save: a trap removed because it was a
      // mistake must stop banning at once, not at the next restart.
      SpamTrapMonitor::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSpamTrap::ReadObject(std::shared_ptr<SpamTrap> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID(pRS->GetInt64Value("stid"));
      pObject->SetDomainID(pRS->GetInt64Value("stdomainid"));
      pObject->SetLocalPart(pRS->GetStringValue("stlocalpart"));
      pObject->SetCreated(Time::GetDateFromSystemDate(pRS->GetStringValue("stcreated")));

      return true;
   }

   bool
   PersistentSpamTrap::SaveObject(std::shared_ptr<SpamTrap> pObject, String &errorMessage, PersistenceMode mode)
   {
      return SaveObject(pObject);
   }

   bool
   PersistentSpamTrap::SaveObject(std::shared_ptr<SpamTrap> pObject)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_spamtraps");

      const bool newObject = pObject->GetID() == 0;

      if (newObject)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("stid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("stid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(where);
      }

      oStatement.AddColumnInt64("stdomainid", pObject->GetDomainID());
      oStatement.AddColumn("stlocalpart", Fit(pObject->GetLocalPart(), 64));

      DateTime created = pObject->GetCreated();
      if (created.GetStatus() != DateTime::valid)
         created = DateTime::GetCurrentTime();

      oStatement.AddColumn("stcreated", Time::GetTimeStampFromDateTime(created));

      __int64 dbid = 0;
      const bool result = Application::Instance()->GetDBManager()->Execute(oStatement, newObject ? &dbid : 0);

      if (result && newObject)
         pObject->SetID(dbid);

      SpamTrapMonitor::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSpamTrap::Find(__int64 domainId, const String &localPart, __int64 &out_id)
   {
      out_id = 0;

      SQLCommand command("select stid from hm_spamtraps where stdomainid = @DOMAINID and stlocalpart = @LOCALPART");
      command.AddParameter("@DOMAINID", domainId);
      command.AddParameter("@LOCALPART", localPart);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return false;

      out_id = recordset->GetInt64Value("stid");

      return out_id != 0;
   }

   bool
   PersistentSpamTrap::DeleteByDomain(__int64 domainId)
   {
      SQLCommand traps("delete from hm_spamtraps where stdomainid = @DOMAINID");
      traps.AddParameter("@DOMAINID", domainId);

      const bool trapsDeleted = Application::Instance()->GetDBManager()->Execute(traps);

      SpamTrapMonitor::SetNeedRefresh();

      return DeleteHits(domainId) && trapsDeleted;
   }

   bool
   PersistentSpamTrap::RecordHit(const SpamTrapHit &hit)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_spamtraphits");
      oStatement.SetStatementType(SQLStatement::STInsert);
      oStatement.SetIdentityColumn("sthid");

      DateTime time = hit.time;
      if (time.GetStatus() != DateTime::valid)
         time = DateTime::GetCurrentTime();

      oStatement.AddColumnInt64("sthdomainid", hit.domain_id);
      oStatement.AddColumn("sthaddress", Fit(hit.address, 255));
      oStatement.AddColumn("sthipaddress", Fit(hit.ip_address, 64));
      oStatement.AddColumn("sthsender", Fit(hit.sender, 255));
      oStatement.AddColumn("sthtime", Time::GetTimeStampFromDateTime(time));
      oStatement.AddColumn("sthbanned", hit.banned ? 1L : 0L);
      oStatement.AddColumn("sthblocked", hit.blocked ? 1L : 0L);
      oStatement.AddColumn("sthnote", Fit(hit.note, 255));

      __int64 dbid = 0;
      return Application::Instance()->GetDBManager()->Execute(oStatement, &dbid);
   }

   bool
   PersistentSpamTrap::ReadHits(__int64 domainId, int maxRows, std::vector<SpamTrapHit> &hits)
   {
      hits.clear();

      if (maxRows < 1)
         maxRows = 1;

      const String where = domainId > 0 ? String(_T(" where sthdomainid = @DOMAINID")) : String();

      String sql;

      switch (IniFileSettings::Instance()->GetDatabaseType())
      {
      case DatabaseSettings::TypeMSSQLServer:
      case DatabaseSettings::TypeMSSQLCompactEdition:
         // TOP (n), with the parentheses SQL Server Compact requires.
         sql.Format(_T("select top (%d) * from hm_spamtraphits%s order by sthtime desc, sthid desc"), maxRows, where.c_str());
         break;
      default:
         sql.Format(_T("select * from hm_spamtraphits%s order by sthtime desc, sthid desc limit %d"), where.c_str(), maxRows);
         break;
      }

      SQLCommand command(sql);
      if (domainId > 0)
         command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         SpamTrapHit hit;
         hit.id = recordset->GetInt64Value("sthid");
         hit.domain_id = recordset->GetInt64Value("sthdomainid");
         hit.address = recordset->GetStringValue("sthaddress");
         hit.ip_address = recordset->GetStringValue("sthipaddress");
         hit.sender = recordset->GetStringValue("sthsender");
         hit.time = Time::GetDateFromSystemDate(recordset->GetStringValue("sthtime"));
         hit.banned = recordset->GetLongValue("sthbanned") == 1;
         hit.blocked = recordset->GetLongValue("sthblocked") == 1;
         hit.note = recordset->GetStringValue("sthnote");

         hits.push_back(hit);

         recordset->MoveNext();
      }

      return true;
   }

   int
   PersistentSpamTrap::CountHits(__int64 domainId)
   {
      SQLCommand command(domainId > 0
         ? String(_T("select count(*) as hitcount from hm_spamtraphits where sthdomainid = @DOMAINID"))
         : String(_T("select count(*) as hitcount from hm_spamtraphits")));

      if (domainId > 0)
         command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return -1;

      return (int) recordset->GetLongValue("hitcount");
   }

   bool
   PersistentSpamTrap::DeleteHits(__int64 domainId)
   {
      SQLCommand command(domainId > 0
         ? String(_T("delete from hm_spamtraphits where sthdomainid = @DOMAINID"))
         : String(_T("delete from hm_spamtraphits")));

      if (domainId > 0)
         command.AddParameter("@DOMAINID", domainId);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentSpamTrap::DeleteHitsOlderThan(int days)
   {
      DateTimeSpan span;
      span.SetDateTimeSpan(days, 0, 0, 0);

      const DateTime cutoff = DateTime::GetCurrentTime() - span;

      SQLCommand command(_T("delete from hm_spamtraphits where sthtime < @TIME"));
      command.AddParameter("@TIME", Time::GetTimeStampFromDateTime(cutoff));

      return Application::Instance()->GetDBManager()->Execute(command);
   }
}
