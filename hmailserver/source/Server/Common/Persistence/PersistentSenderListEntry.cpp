// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentSenderListEntry.h"
#include "../BO/SenderListEntry.h"
#include "../AntiSpam/SenderListCache.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentSenderListEntry::PersistentSenderListEntry()
   {
   }

   PersistentSenderListEntry::~PersistentSenderListEntry()
   {
   }

   bool
   PersistentSenderListEntry::DeleteObject(std::shared_ptr<SenderListEntry> pObject)
   {
      SQLCommand command("delete from hm_senderlist where slid = @SLID");
      command.AddParameter("@SLID", pObject->GetID());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      // On the delete as well as on the save. A cache that is told about saves
      // and not about deletes keeps an entry alive until a restart, and the
      // entry people most want gone at once is a block they have just decided
      // was wrong.
      SenderListCache::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSenderListEntry::ReadObject(std::shared_ptr<SenderListEntry> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID(pRS->GetInt64Value("slid"));
      pObject->SetScope(pRS->GetLongValue("slscope"));
      pObject->SetOwnerID(pRS->GetInt64Value("slownerid"));
      pObject->SetKind(pRS->GetLongValue("sltype"));
      pObject->SetValue(pRS->GetStringValue("slvalue"));
      pObject->SetReason(pRS->GetStringValue("slreason"));
      pObject->SetExpires(pRS->GetLongValue("slexpires") == 1);
      pObject->SetExpiresTime(Time::GetDateFromSystemDate(pRS->GetStringValue("slexpirestime")));
      pObject->SetCreated(Time::GetDateFromSystemDate(pRS->GetStringValue("slcreated")));

      return true;
   }

   bool
   PersistentSenderListEntry::SaveObject(std::shared_ptr<SenderListEntry> pObject, String &errorMessage, PersistenceMode mode)
   {
      return SaveObject(pObject);
   }

   bool
   PersistentSenderListEntry::SaveObject(std::shared_ptr<SenderListEntry> pObject)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_senderlist");

      const bool newObject = pObject->GetID() == 0;

      if (newObject)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("slid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("slid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(where);
      }

      oStatement.AddColumn("slscope", (long) pObject->GetScope());
      oStatement.AddColumnInt64("slownerid", pObject->GetOwnerID());
      oStatement.AddColumn("sltype", (long) pObject->GetKind());
      oStatement.AddColumn("slvalue", pObject->GetValue());
      oStatement.AddColumn("slreason", pObject->GetReason());
      oStatement.AddColumn("slexpires", pObject->GetExpires() ? 1L : 0L);

      // Written whether or not the entry expires, because the column is NOT
      // NULL and because an expiry left at the OLE epoch on a row that does not
      // expire would be a date in 1899 waiting for somebody to switch the flag
      // on: the sweep would then take the entry away in the same minute. A
      // non-expiring entry stores the moment it was written, which is
      // meaningless and harmless, and HasExpired never looks at it.
      DateTime expiresTime = pObject->GetExpiresTime();
      if (expiresTime.GetStatus() != DateTime::valid)
         expiresTime = DateTime::GetCurrentTime();

      oStatement.AddColumn("slexpirestime", Time::GetTimeStampFromDateTime(expiresTime));

      DateTime created = pObject->GetCreated();
      if (created.GetStatus() != DateTime::valid)
         created = DateTime::GetCurrentTime();

      oStatement.AddColumn("slcreated", Time::GetTimeStampFromDateTime(created));

      __int64 dbid = 0;
      const bool result = Application::Instance()->GetDBManager()->Execute(oStatement, newObject ? &dbid : 0);

      if (result && newObject)
         pObject->SetID(dbid);

      SenderListCache::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSenderListEntry::DeleteByOwner(int scope, __int64 ownerId)
   {
      SQLCommand command("delete from hm_senderlist where slscope = @SCOPE and slownerid = @OWNERID");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@OWNERID", ownerId);

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      SenderListCache::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSenderListEntry::DeleteExpired()
   {
      SQLCommand command(_T("delete from hm_senderlist where slexpires = 1 AND slexpirestime < @TIME"));
      command.AddParameter("@TIME", Time::GetCurrentDateTime());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      SenderListCache::SetNeedRefresh();

      return result;
   }

   bool
   PersistentSenderListEntry::Find(int scope, __int64 ownerId, const String &value, __int64 &out_id)
   {
      out_id = 0;

      SQLCommand command("select slid from hm_senderlist where slscope = @SCOPE and slownerid = @OWNERID and slvalue = @VALUE");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@OWNERID", ownerId);
      command.AddParameter("@VALUE", value);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return false;

      out_id = recordset->GetInt64Value("slid");

      return out_id != 0;
   }
}
