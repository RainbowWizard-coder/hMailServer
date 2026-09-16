// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentGreyListingWhiteAddress.h"
#include "../BO/GreyListingWhiteAddress.h"
#include "../SQL/SQLStatement.h"
#include "../Util/AuditTrail.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentGreyListingWhiteAddress::PersistentGreyListingWhiteAddress(void)
   {
   }

   PersistentGreyListingWhiteAddress::~PersistentGreyListingWhiteAddress(void)
   {
   }

   bool
   PersistentGreyListingWhiteAddress::DeleteObject(std::shared_ptr<GreyListingWhiteAddress> pObject)
   {
      SQLCommand command("delete from hm_greylisting_whiteaddresses where whiteid = @WHITEID");
      command.AddParameter("@WHITEID", pObject->GetID());

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool 
   PersistentGreyListingWhiteAddress::ReadObject(std::shared_ptr<GreyListingWhiteAddress> pObject, std::shared_ptr<DALRecordset> pRS)
   {
      pObject->SetID (pRS->GetLongValue("whiteid"));
      pObject->SetIPAddress(pRS->GetStringValue("whiteipaddress"));
      pObject->SetDescription(pRS->GetStringValue("whiteipdescription"));

      return true;
   }

   bool
   PersistentGreyListingWhiteAddress::SaveObject(std::shared_ptr<GreyListingWhiteAddress> pObject)
   {
      String errorMessage;
      return SaveObject(pObject, errorMessage, PersistenceModeNormal);
   }

   bool
   PersistentGreyListingWhiteAddress::SaveObject(std::shared_ptr<GreyListingWhiteAddress> pObject, String &errorMessage, PersistenceMode mode)
   {
      // The address is the pattern IsSenderWhitelisted matches the connecting
      // address against; an empty one matches nothing and is a row that
      // does nothing. Refused here, where the value is owned, so the COM item
      // and the REST route meet the same sentence.
      String address = pObject->GetIPAddress();
      address.Trim();
      if (address.IsEmpty())
      {
         errorMessage = "The IP address must not be empty.";
         return false;
      }

      SQLStatement oStatement;
      oStatement.SetTable("hm_greylisting_whiteaddresses");
      
      if (pObject->GetID() == 0)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("whiteid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);
         String sWhere;
         sWhere.Format(_T("whiteid = %I64d"), pObject->GetID());
         oStatement.SetWhereClause(sWhere);
         
      }

      oStatement.AddColumn("whiteipaddress", pObject->GetIPAddress());
      oStatement.AddColumn("whiteipdescription", pObject->GetDescription());

      bool bNewObject = pObject->GetID() == 0;

      // The trail names this row as the administrator typed it. The column holds
      // the SQL LIKE pattern SetUserEditableIPAddress made of what they typed -
      // 10.78.0.? is stored as 10.78.0._, and a literal _ as /_ - and the recorder
      // names an object by the value the statement carried, which is what keeps it
      // honest everywhere else and what made it unrecognisable here: the Control
      // Panel, the REST list and hmctl all show the wildcard form, so a reader
      // matching the trail against the configuration found an address that is in
      // neither. This is the one column in the schema that stores an encoding of
      // what a person typed, and this is the place that writes it. Only the name:
      // the detail is still the columns, so the stored pattern is on the record too.
      AuditDescription described(_T("hm_greylisting_whiteaddresses"), pObject->GetUserEditableIPAddress());

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0);
      if (bRetVal && bNewObject)
         pObject->SetID((int) iDBID);

      // bRetVal, not true. This computed whether the statement had succeeded and then
      // reported success regardless, so every caller that checked was checking a
      // constant - including the ones hardened earlier this week, whose checks were
      // therefore correct code that could never fire. Found by the database fault
      // injector on its first run, which is the entire argument for having built it.
      return bRetVal;
   }

   /// Checks if a specific sender on a specific IP address is white listed.
   bool 
   PersistentGreyListingWhiteAddress::IsSenderWhitelisted(const String &address)
   {
     SQLCommand command1("SELECT COUNT(*) as c FROM hm_greylisting_whiteaddresses "
         "WHERE @ADDRESS LIKE whiteipaddress ESCAPE '/'");

      command1.AddParameter("@ADDRESS", address);
   
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command1);
      
      if (!pRS)
         return false;

      long count = pRS->GetLongValue("c");

      if (count > 0)
         return true;
      else 
         return false;
   }
}