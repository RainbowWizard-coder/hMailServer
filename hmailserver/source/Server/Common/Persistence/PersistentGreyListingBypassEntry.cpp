// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentGreyListingBypassEntry.h"
#include "../BO/GreyListingBypassEntry.h"
#include "../SQL/SQLStatement.h"
#include "../AntiSpam/GreyListingBypass.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentGreyListingBypassEntry::PersistentGreyListingBypassEntry(void)
   {
   }

   PersistentGreyListingBypassEntry::~PersistentGreyListingBypassEntry(void)
   {
   }

   bool
   PersistentGreyListingBypassEntry::DeleteObject(std::shared_ptr<GreyListingBypassEntry> pObject)
   {
      SQLCommand command("delete from hm_greylisting_bypass where glbid = @GLBID");
      command.AddParameter("@GLBID", pObject->GetID());

      bool bResult = Application::Instance()->GetDBManager()->Execute(command);
      GreyListingBypass::SetNeedRefresh();
      return bResult;
   }

   bool
   PersistentGreyListingBypassEntry::ReadObject(std::shared_ptr<GreyListingBypassEntry> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID(pRS->GetInt64Value("glbid"));
      pObject->SetPattern(pRS->GetStringValue("glbpattern"));
      pObject->SetMatchOn(pRS->GetLongValue("glbmatch"));
      pObject->SetRequire(pRS->GetLongValue("glbrequire"));
      pObject->SetNote(pRS->GetStringValue("glbnote"));

      return true;
   }

   bool
   PersistentGreyListingBypassEntry::SaveObject(std::shared_ptr<GreyListingBypassEntry> pObject)
   {
      String errorMessage;
      return SaveObject(pObject, errorMessage, PersistenceModeNormal);
   }

   bool
   PersistentGreyListingBypassEntry::SaveObject(std::shared_ptr<GreyListingBypassEntry> pObject, String &errorMessage, PersistenceMode mode)
   {
      // SetPattern has trimmed and lower-cased it. A leading dot is the suffix
      // form; what follows the dot, or the whole of it, has to be a domain name,
      // because a row nothing can match is a row that grants nothing and says
      // otherwise.
      const String pattern = pObject->GetPattern();
      if (pattern.IsEmpty())
      {
         errorMessage = "The pattern must not be empty: a domain such as example.com, or a suffix such as .mail.example.com.";
         return false;
      }

      const String bare = pattern[0] == _T('.') ? pattern.Mid(1) : pattern;
      if (bare.IsEmpty() || bare[0] == _T('[') || !StringParser::IsValidDomainName(bare))
      {
         errorMessage = "The pattern is a domain name such as example.com, or a suffix such as .mail.example.com; not an address, not an IP address.";
         return false;
      }

      const int matchOn = pObject->GetMatchOn();
      if (matchOn != GreyListingBypassEntry::MatchEnvelope &&
          matchOn != GreyListingBypassEntry::MatchHelo &&
          matchOn != GreyListingBypassEntry::MatchBoth)
      {
         errorMessage = "The pattern is matched against the envelope sender's domain, the HELO host, or both.";
         return false;
      }

      // The note's column is 255 characters. Refused rather than cut, as the
      // pattern is: the create's reply echoes what was sent, so a silent
      // truncation answers with text the row does not hold.
      if (pObject->GetNote().GetLength() > 255)
      {
         errorMessage = "The note is at most 255 characters.";
         return false;
      }

      const int allRequirements = GreyListingBypassEntry::RequireSpf | GreyListingBypassEntry::RequireDkim | GreyListingBypassEntry::RequireDmarc;
      const int require = pObject->GetRequire();
      if (require == 0 || (require & ~allRequirements) != 0)
      {
         errorMessage = "At least one of SPF, DKIM and DMARC must be required. A domain to be let past greylisting on no evidence at all belongs on the greylisting white list by its addresses, not here.";
         return false;
      }

      SQLStatement oStatement;
      oStatement.SetTable("hm_greylisting_bypass");

      if (pObject->GetID() == 0)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("glbid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);
         String sWhere;
         sWhere.Format(_T("glbid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(sWhere);
      }

      oStatement.AddColumn("glbpattern", pattern.Left(255));
      oStatement.AddColumnInt64("glbmatch", matchOn);
      oStatement.AddColumnInt64("glbrequire", require);
      oStatement.AddColumn("glbnote", pObject->GetNote());

      bool bNewObject = pObject->GetID() == 0;

      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0);
      if (bRetVal && bNewObject)
         pObject->SetID(iDBID);

      GreyListingBypass::SetNeedRefresh();

      return bRetVal;
   }
}
