// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentNeverBanName.h"
#include "../BO/NeverBanName.h"
#include "../Util/NeverBanNameList.h"
#include "../Util/AuditTrail.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"
#include "../Util/Time.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentNeverBanName::PersistentNeverBanName()
   {
   }

   PersistentNeverBanName::~PersistentNeverBanName()
   {
   }

   bool
   PersistentNeverBanName::DeleteObject(std::shared_ptr<NeverBanName> pObject)
   {
      SQLCommand command("delete from hm_neverbannames where nbnid = @NBNID");
      command.AddParameter("@NBNID", pObject->GetID());

      // The statement carries the row's id alone, so the trail is told which
      // pattern went: the one thing a reader of it asks about a never-ban row.
      AuditDescription described(_T("hm_neverbannames"), pObject->GetPattern());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      // On the delete as well as on the save: a pattern removed because it was a
      // mistake must stop sparing anybody at once, not at the next restart.
      NeverBanNameList::SetNeedRefresh();

      return result;
   }

   bool
   PersistentNeverBanName::ReadObject(std::shared_ptr<NeverBanName> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID(pRS->GetInt64Value("nbnid"));
      pObject->SetPattern(pRS->GetStringValue("nbnpattern"));
      pObject->SetNote(pRS->GetStringValue("nbnnote"));
      pObject->SetCreated(Time::GetDateFromSystemDate(pRS->GetStringValue("nbncreated")));

      return true;
   }

   bool
   PersistentNeverBanName::SaveObject(std::shared_ptr<NeverBanName> pObject, String &errorMessage, PersistenceMode mode)
   {
      String refusal;
      if (!NeverBanName::ValidatePattern(pObject->GetPattern(), refusal))
      {
         errorMessage = refusal;
         return false;
      }

      if (pObject->GetNote().GetLength() > 255)
      {
         errorMessage = _T("note is at most 255 characters");
         return false;
      }

      const __int64 existing = Find(pObject->GetPattern());
      if (existing != 0 && existing != pObject->GetID())
      {
         errorMessage = _T("the list already has ") + pObject->GetPattern() + _T("; change that entry rather than adding a second");
         return false;
      }

      return SaveObject(pObject);
   }

   bool
   PersistentNeverBanName::SaveObject(std::shared_ptr<NeverBanName> pObject)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_neverbannames");

      const bool newObject = pObject->GetID() == 0;

      if (newObject)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("nbnid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("nbnid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(where);
      }

      oStatement.AddColumn("nbnpattern", pObject->GetPattern(), 255);
      oStatement.AddColumn("nbnnote", pObject->GetNote(), 255);

      // Stamped here whatever the caller did, so a restore, a script or the REST
      // API leaving it empty does not fail the insert on the NOT NULL column.
      DateTime created = pObject->GetCreated();
      if (created.GetStatus() != DateTime::valid)
         created = DateTime::GetCurrentTime();

      oStatement.AddColumn("nbncreated", Time::GetTimeStampFromDateTime(created));

      // Named by its pattern: the column is not called ...name, so the trail
      // would otherwise name an update by its row id.
      AuditDescription described(_T("hm_neverbannames"), pObject->GetPattern());

      __int64 dbid = 0;
      const bool result = Application::Instance()->GetDBManager()->Execute(oStatement, newObject ? &dbid : 0);

      if (result && newObject)
         pObject->SetID(dbid);

      NeverBanNameList::SetNeedRefresh();

      return result;
   }

   __int64
   PersistentNeverBanName::Find(const String &pattern)
   {
      // nbnpattern is nvarchar(255). SQL Server Compact fails a comparison with a
      // longer parameter rather than finding nothing, so a longer pattern - which
      // ValidatePattern refuses before anything is saved - is not asked about.
      if (pattern.IsEmpty() || pattern.GetLength() > 255)
         return 0;

      SQLCommand command("select nbnid from hm_neverbannames where nbnpattern = @PATTERN");
      command.AddParameter("@PATTERN", pattern);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return 0;

      return recordset->GetInt64Value("nbnid");
   }
}
