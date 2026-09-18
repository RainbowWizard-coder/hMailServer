// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentDomainPolicyEntry.h"
#include "../BO/DomainPolicyEntry.h"
#include "../AntiSpam/DomainPolicyList.h"
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
   PersistentDomainPolicyEntry::PersistentDomainPolicyEntry()
   {
   }

   PersistentDomainPolicyEntry::~PersistentDomainPolicyEntry()
   {
   }

   bool
   PersistentDomainPolicyEntry::DeleteObject(std::shared_ptr<DomainPolicyEntry> pObject)
   {
      SQLCommand command("delete from hm_domainpolicylist where dplid = @DPLID");
      command.AddParameter("@DPLID", pObject->GetID());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      // On the delete as well as on the save: a black entry removed because it
      // was a mistake must stop refusing at once, not at the next restart.
      DomainPolicyList::SetNeedRefresh();

      return result;
   }

   bool
   PersistentDomainPolicyEntry::ReadObject(std::shared_ptr<DomainPolicyEntry> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID(pRS->GetInt64Value("dplid"));
      pObject->SetPattern(pRS->GetStringValue("dplpattern"));

      const long matchOn = pRS->GetLongValue("dplmatchon");
      pObject->SetMatchOn(matchOn == DomainPolicyEntry::MatchEnvelope ? DomainPolicyEntry::MatchEnvelope :
                          matchOn == DomainPolicyEntry::MatchHelo ? DomainPolicyEntry::MatchHelo :
                          DomainPolicyEntry::MatchBoth);

      // Anything that is not the black value is white: the row that refuses mail
      // is the one a stray value must never turn into.
      pObject->SetAction(pRS->GetLongValue("dplaction") == DomainPolicyEntry::ActionBlack ? DomainPolicyEntry::ActionBlack : DomainPolicyEntry::ActionWhite);

      pObject->SetNote(pRS->GetStringValue("dplnote"));
      pObject->SetCreated(Time::GetDateFromSystemDate(pRS->GetStringValue("dplcreated")));

      return true;
   }

   bool
   PersistentDomainPolicyEntry::SaveObject(std::shared_ptr<DomainPolicyEntry> pObject, String &errorMessage, PersistenceMode mode)
   {
      String refusal;
      if (!DomainPolicyEntry::ValidatePattern(pObject->GetPattern(), refusal))
      {
         errorMessage = refusal;
         return false;
      }

      // The (pattern, match key) pair is unique. Found here, in words, rather than
      // left to the index: a constraint violation is reported as a server error
      // and reads to a caller as "could not be saved", which says nothing about
      // the row that is already there.
      const __int64 existing = Find(pObject->GetPattern(), (int) pObject->GetMatchOn());
      if (existing != 0 && existing != pObject->GetID())
      {
         errorMessage = _T("the list already has ") + pObject->GetPattern() + _T(" matched on ") +
                        DomainPolicyEntry::MatchOnWord(pObject->GetMatchOn()) + _T("; change that entry rather than adding a second");
         return false;
      }

      return SaveObject(pObject);
   }

   bool
   PersistentDomainPolicyEntry::SaveObject(std::shared_ptr<DomainPolicyEntry> pObject)
   {
      SQLStatement oStatement;
      oStatement.SetTable("hm_domainpolicylist");

      const bool newObject = pObject->GetID() == 0;

      if (newObject)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("dplid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("dplid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(where);
      }

      oStatement.AddColumn("dplpattern", pObject->GetPattern(), 255);
      oStatement.AddColumn("dplmatchon", (long) pObject->GetMatchOn());
      oStatement.AddColumn("dplaction", (long) pObject->GetAction());
      oStatement.AddColumn("dplnote", pObject->GetNote(), 255);

      DateTime created = pObject->GetCreated();
      if (created.GetStatus() != DateTime::valid)
         created = DateTime::GetCurrentTime();

      oStatement.AddColumn("dplcreated", Time::GetTimeStampFromDateTime(created));

      __int64 dbid = 0;
      const bool result = Application::Instance()->GetDBManager()->Execute(oStatement, newObject ? &dbid : 0);

      if (result && newObject)
         pObject->SetID(dbid);

      DomainPolicyList::SetNeedRefresh();

      return result;
   }

   __int64
   PersistentDomainPolicyEntry::Find(const String &pattern, int matchOn)
   {
      // dplpattern is nvarchar(255). SQL Server Compact fails a comparison with a
      // longer parameter rather than finding nothing, so a longer pattern - which
      // ValidatePattern refuses before anything is saved - is not asked about.
      if (pattern.IsEmpty() || pattern.GetLength() > 255)
         return 0;

      SQLCommand command("select dplid from hm_domainpolicylist where dplpattern = @PATTERN and dplmatchon = @MATCHON");
      command.AddParameter("@PATTERN", pattern);
      command.AddParameter("@MATCHON", matchOn);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

      if (!recordset || recordset->IsEOF())
         return 0;

      return recordset->GetInt64Value("dplid");
   }
}
