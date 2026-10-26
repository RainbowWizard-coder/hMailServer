// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "PersistentSecurityRange.h"
#include "PersistenceMode.h"

#include "../BO/SecurityRange.h"

#include "../Util/Time.h"
#include "../Util/AutoBanFirewall.h"

#include "../SQL/IPAddressSQLHelper.h"
#include "PreSaveLimitationsCheck.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentSecurityRange::PersistentSecurityRange()
   {

   }

   PersistentSecurityRange::~PersistentSecurityRange()
   {

   }

   bool
   PersistentSecurityRange::DeleteObject(std::shared_ptr<SecurityRange> pSR)
   {
      HM_ASSERT(pSR->GetID());

      bool bResult = false;
      if (pSR->GetID() > 0)
      {
         SQLCommand command("delete from hm_securityranges where rangeid = @RANGEID");
         command.AddParameter("@RANGEID", pSR->GetID());
         
         bResult = Application::Instance()->GetDBManager()->Execute(command);

         // Deleting the range is how an administrator lifts an auto-ban early.
         // The firewall follows the table, so it is told now rather than on the
         // next expiry pass.
         if (bResult)
            AutoBanFirewall::Synchronise(_T("range deleted"));
      }

      return bResult;

   }

   bool
   PersistentSecurityRange::SaveObject(std::shared_ptr<SecurityRange> pSR)
   {
      String result;

      return SaveObject(pSR, result, PersistenceModeNormal);
   }

   bool
   PersistentSecurityRange::SaveObject(std::shared_ptr<SecurityRange> pSR, String &result,  PersistenceMode mode)
   {
      return Save_(pSR, result, mode, nullptr);
   }

   bool
   PersistentSecurityRange::SaveNewObject(std::shared_ptr<SecurityRange> pSR, bool &nameTaken)
   {
      nameTaken = false;

      String result;
      return Save_(pSR, result, PersistenceModeNormal, &nameTaken);
   }

   String
   PersistentSecurityRange::CutName(const String &text, int units)
   {
      if (units <= 0)
         return String();

      if (text.GetLength() <= units)
         return text;

      // The unit kept last is the first half of a surrogate pair: it goes with
      // its second half. A wchar_t of 32 bits holds no such half in text that
      // was decoded, and one that holds it is cut the same way.
      int keep = units;
      const wchar_t last = text[keep - 1];
      if (last >= 0xD800 && last <= 0xDBFF)
         keep--;

      return text.Mid(0, keep);
   }

   String
   PersistentSecurityRange::StoredName(const String &name)
   {
      return CutName(name, MaxNameLength);
   }

   bool
   PersistentSecurityRange::Save_(std::shared_ptr<SecurityRange> pSR, String &result, PersistenceMode mode, bool *nameTaken)
   {
      if (nameTaken != nullptr)
         *nameTaken = false;

      if (!Validate(pSR, result))
         return false;

      // The limitation check's one question about a range is whether a range has
      // its name already. SaveNewObject's caller chose the name as free a moment
      // ago, and the unique index is what decides - between servers sharing the
      // database too, which the check cannot - so it is asked there alone, and its
      // refusal is what nameTaken says.
      if (nameTaken == nullptr && !PreSaveLimitationsCheck::CheckLimitations(mode, pSR, result))
         return false;

      DateTime rangeExpiresTime = pSR->GetExpiresTime();
      if (rangeExpiresTime.GetStatus() != DateTime::valid)
         rangeExpiresTime.SetDateTime(2001,01,01,0,0,0);

      // The name the column stores, and the object is given it, so that what the
      // caller reads back and reports is the row's name. Up to 6.3.7 the cut was
      // made here alone, at 100 units whatever they were, while the auto-ban's
      // choice of a free name asked about the name before the cut.
      const String name = StoredName(pSR->GetName());
      pSR->SetName(name);

      IPAddressSQLHelper helper;

      SQLStatement oStatement;
      oStatement.SetTable("hm_securityranges");

      oStatement.AddColumn("rangename", name);
      oStatement.AddColumn("rangepriorityid", pSR->GetPriority());

      helper.AppendStatement(oStatement, pSR->GetLowerIP(), "rangelowerip1", "rangelowerip2");
      helper.AppendStatement(oStatement, pSR->GetUpperIP(), "rangeupperip1", "rangeupperip2");

      oStatement.AddColumn("rangeoptions", pSR->GetOptions());
      oStatement.AddColumn("rangeexpires", pSR->GetExpires());
      oStatement.AddColumn("rangeexpirestime", Time::GetTimeStampFromDateTime(rangeExpiresTime));

      if (pSR->GetID() == 0)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("rangeid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String sWhere;
         sWhere.Format(_T("rangeid = %I64d"), pSR->GetID());
         oStatement.SetWhereClause(sWhere);
      }


      bool bNewObject = pSR->GetID() == 0;

      // A refusal by the database's own rules is forgiven only for an insert a
      // caller asked to be told of one (SaveNewObject), and an insert forgiven is
      // answered as a success with no id (DALConnection::TryExecute): the id is
      // what tells a row written from one refused.
      const bool forgive = nameTaken != nullptr && bNewObject;

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0, forgive ? DALConnection::DALErrorInSQL : 0);
      if (bRetVal && forgive && iDBID <= 0)
      {
         *nameTaken = true;
         result = "A range of this name exists already.";
         return false;
      }

      if (bRetVal && bNewObject)
         pSR->SetID((int) iDBID);

      if (!bRetVal)
         result = "Failed to save. Please see the hMailServer error log for details.";

      return bRetVal;
   }

   bool
   PersistentSecurityRange::ReadObject(std::shared_ptr<SecurityRange> pSR, __int64 lDBID)
   {
      SQLCommand command(_T("select * from hm_securityranges where rangeid = @RANGEID"));
      command.AddParameter("@RANGEID", lDBID);

      return ReadObject(pSR, command);
   }

   bool
   PersistentSecurityRange::ReadObject(std::shared_ptr<SecurityRange> pSR, const String &name)
   {
      SQLCommand command(_T("select * from hm_securityranges where rangename = @RANGENAME"));
      command.AddParameter("@RANGENAME", StoredName(name));

      return ReadObject(pSR, command);
   }

   bool
   PersistentSecurityRange::ReadObject(std::shared_ptr<SecurityRange> pSR, const SQLCommand &command)
   {
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return false;

      bool bRetVal = false;
      if (!pRS->IsEOF())
      {
         bRetVal = ReadObject(pSR, pRS);
      }
  
      return bRetVal;
   }



   bool
   PersistentSecurityRange::ReadObject(std::shared_ptr<SecurityRange> pSR, std::shared_ptr<DALRecordset> pRS)
   {
      IPAddressSQLHelper helper;

      pSR->SetID(pRS->GetLongValue("rangeid"));
      pSR->SetPriority(pRS->GetLongValue("rangepriorityid"));
      pSR->SetLowerIP(helper.Construct(pRS, "rangelowerip1", "rangelowerip2"));
      pSR->SetUpperIP(helper.Construct(pRS, "rangeupperip1", "rangeupperip2"));
      pSR->SetOptions(pRS->GetLongValue("rangeoptions"));
      pSR->SetName(pRS->GetStringValue("rangename"));
      
      pSR->SetExpires(pRS->GetLongValue("rangeexpires") == 1);
      pSR->SetExpiresTime(Time::GetDateFromSystemDate(pRS->GetStringValue("rangeexpirestime")));

      return true;
   }

   std::shared_ptr<SecurityRange>
   PersistentSecurityRange::ReadMatchingIP(const IPAddress &ipaddress)
   {
      std::shared_ptr<SecurityRange> empty;

      IPAddressSQLHelper helper;
      String sSQL;

      if (ipaddress.GetType() == IPAddress::IPV4)
      {
         std::shared_ptr<SecurityRange> pSR = std::shared_ptr<SecurityRange>(new SecurityRange());

         sSQL.Format(_T("select * from hm_securityranges where %s >= rangelowerip1 and %s <= rangeupperip1 and rangelowerip2 IS NULL and rangeupperip2 IS NULL order by rangepriorityid desc"), 
            String(helper.GetAddress1String(ipaddress)).c_str(), String(helper.GetAddress1String(ipaddress)).c_str());

         if (!ReadObject(pSR, SQLCommand(sSQL)))
            return empty;

         return pSR;
      }
      else
      {
         // Read all IPv6 items.
         std::shared_ptr<SecurityRange> bestMatch;

         SQLCommand command(_T("select * from hm_securityranges where rangelowerip2 is not null order by rangepriorityid desc"));
         
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return empty;

         while (!recordset->IsEOF())
         {
            std::shared_ptr<SecurityRange> securityRange = std::shared_ptr<SecurityRange>(new SecurityRange());

            if (ReadObject(securityRange, recordset) == false)
               return empty;

            if (ipaddress.WithinRange(securityRange->GetLowerIP(), securityRange->GetUpperIP()))
            {
               // This IP range matches the client. Does it have higher prio than the currently
               // matching?

               if (!bestMatch || securityRange->GetPriority() > bestMatch->GetPriority())
                  bestMatch = securityRange;
            }

            recordset->MoveNext();
         }

         return bestMatch;
      }


      
   }


   bool 
   PersistentSecurityRange::DeleteExpired()
   {
      SQLCommand command(_T("delete from hm_securityranges where rangeexpires = 1 AND rangeexpirestime < @TIME"));
      command.AddParameter("@TIME", Time::GetCurrentDateTime());

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentSecurityRange::Exists(const String &name)
   {
      // Bound as a parameter, as ReadObject(name) binds it and SaveObject's
      // INSERT binds the name. Up to 6.3.7 the name was written into the WHERE
      // clause as a '...' literal, which SQL Server reads as varchar in the
      // database's code page: a name with a character outside it compared
      // unequal to the nvarchar the INSERT had stored under it, so the auto-ban's
      // choice of a free name chose the stored one again and the unique index
      // refused the range. MySQL and PostgreSQL are given the literal the INSERT
      // is written with (SQLStatement::GenerateFromCommand), as before.
      SQLCommand command(_T("select count(*) as c from hm_securityranges where rangename = @RANGENAME"));
      command.AddParameter("@RANGENAME", StoredName(name));

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return false;

      if (pRS->IsEOF())
         return false;

      return pRS->GetLongValue("c") > 0;
   }

   bool
   PersistentSecurityRange::WidenAutoBan(__int64 id, const IPAddress &address, const IPAddress &lower, const IPAddress &upper)
   {
      if (id <= 0 || address.GetType() != IPAddress::IPV4 || lower.GetType() != IPAddress::IPV4 || upper.GetType() != IPAddress::IPV4)
         return false;

      const __int64 was = (__int64) address.GetAddress1();
      const __int64 newLower = (__int64) lower.GetAddress1();
      const __int64 newUpper = (__int64) upper.GetAddress1();

      if (newLower > was || newUpper < was)
         return false;

      // Conditional on everything the row was when it was read, so that an
      // administrator's edit or a lift made while the neighbours were looked up
      // wins. No parameter's name begins another's.
      SQLCommand command(_T("update hm_securityranges set rangelowerip1 = @NEWLOWER, rangeupperip1 = @NEWUPPER ")
                         _T("where rangeid = @RANGEID and rangelowerip1 = @WASLOWER and rangeupperip1 = @WASUPPER ")
                         _T("and rangelowerip2 is null and rangeupperip2 is null and rangeexpires = 1"));
      command.AddParameter("@NEWLOWER", newLower);
      command.AddParameter("@NEWUPPER", newUpper);
      command.AddParameter("@RANGEID", id);
      command.AddParameter("@WASLOWER", was);
      command.AddParameter("@WASUPPER", was);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentSecurityRange::Validate(std::shared_ptr<SecurityRange> pSR, String &result)
   {
      if (pSR->GetName().IsEmpty())
      {
         result = "The name cannot be empty.";
         return false;
      }

      if (pSR->GetLowerIP().GetType() != pSR->GetUpperIP().GetType())
      {
         result = "The lower IP address and upper IP address must be of the same IP version type.";
         return false;
      }
      
      if (pSR->GetLowerIP() > pSR->GetUpperIP())
      {
         result = "The lower IP address must be lower or the same as the upper IP address.";
         return false;
      }

      // A range that expires without a time to expire at would be written
      // with the placeholder SaveObject uses for a time it has none of,
      // 2001-01-01, and DeleteExpired would remove it before it was ever
      // consulted. The auto-ban always sets both; the desktop dialog and the
      // REST routes are told, in this sentence, when they set only the flag.
      if (pSR->GetExpires() && pSR->GetExpiresTime().GetStatus() != DateTime::valid)
      {
         result = "An expiring range needs an expiry time.";
         return false;
      }

      return true;
   }
}