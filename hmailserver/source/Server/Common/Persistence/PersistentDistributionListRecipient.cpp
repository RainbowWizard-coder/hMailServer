// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "PersistentDistributionListRecipient.h"

#include "../Cache/Cache.h"

#include "PersistentDistributionList.h"
#include "PreSaveLimitationsCheck.h"
#include "PersistenceMode.h"

#include "../BO/DistributionList.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentDistributionListRecipient::PersistentDistributionListRecipient(void)
   {
   }

   PersistentDistributionListRecipient::~PersistentDistributionListRecipient(void)
   {
   }

   bool
   PersistentDistributionListRecipient::DeleteObject(std::shared_ptr<DistributionListRecipient> pRecipient)
   {
      if (pRecipient->GetID() == 0)
      {
         HM_ASSERT(0);
         return false;
      }

      SQLCommand command("delete from hm_distributionlistsrecipients where distributionlistrecipientid = @RECIPIENTID");
      command.AddParameter("@RECIPIENTID", pRecipient->GetID());

      bool bResult = Application::Instance()->GetDBManager()->Execute(command);

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return bResult;
   }

   bool 
   PersistentDistributionListRecipient::DeleteByListID(__int64 iListID)
   {
      SQLCommand command("delete from hm_distributionlistsrecipients where distributionlistrecipientlistid = @LISTID");
      command.AddParameter("@LISTID", iListID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentDistributionListRecipient::ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, long ObjectID)
   {
      SQLCommand selectCommand(_T("select * from hm_distributionlistsrecipients where distributionlistrecipientid = @RECIPIENTID"));
      // ObjectID is a long. AddParameter is overloaded on int, unsigned int and
      // __int64, and the overload decides the type of the SQL parameter that is
      // bound, so the choice is not cosmetic. MSVC binds a long to the int overload,
      // because long and int are the same 32-bit type on Windows; clang finds all
      // three equally distant. The cast names the overload Windows already selects.
      selectCommand.AddParameter("@RECIPIENTID", (int) ObjectID);

      return ReadObject(pRecipient, selectCommand);
   }

   bool
   PersistentDistributionListRecipient::ReadByToken(std::shared_ptr<DistributionListRecipient> pRecipient, const String &token)
   {
      // Every row's token is its own random value, and an empty one - a member
      // the manager has not needed to give one yet - is never a match.
      if (token.IsEmpty())
         return false;

      SQLCommand selectCommand(_T("select * from hm_distributionlistsrecipients where distributionlistrecipienttoken = @TOKEN"));
      selectCommand.AddParameter("@TOKEN", token);

      return ReadObject(pRecipient, selectCommand);
   }

   bool
   PersistentDistributionListRecipient::ReadByAddress(std::shared_ptr<DistributionListRecipient> pRecipient, __int64 listId, const String &address)
   {
      if (address.IsEmpty())
         return false;

      // The list's rows, compared here rather than in the statement: an
      // address is compared without regard to case, and PostgreSQL's equality
      // is not, while the other three backends' is. A list is its members, so
      // this reads what the expansion at RCPT TO reads anyway.
      SQLCommand selectCommand(_T("select * from hm_distributionlistsrecipients where distributionlistrecipientlistid = @LISTID"));
      selectCommand.AddParameter("@LISTID", listId);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(selectCommand);
      if (!pRS)
         return false;

      while (!pRS->IsEOF())
      {
         if (pRS->GetStringValue("distributionlistrecipientaddress").CompareNoCase(address) == 0)
            return ReadObject(pRecipient, pRS);

         pRS->MoveNext();
      }

      return false;
   }

   bool
   PersistentDistributionListRecipient::ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, const SQLCommand &command)
   {
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);

      // A database that did not answer is not an empty table.
      if (!pRS)
         return false;

      bool bRetVal = false;
      if (!pRS->IsEOF())
      {
         bRetVal = ReadObject(pRecipient, pRS);
      }

      return bRetVal;
   }



   bool
   PersistentDistributionListRecipient::ReadObject(std::shared_ptr<DistributionListRecipient> pRecipient, std::shared_ptr<DALRecordset> pRS)
   {
      pRecipient->SetID(pRS->GetLongValue("distributionlistrecipientid"));
      pRecipient->SetListID(pRS->GetLongValue("distributionlistrecipientlistid"));
      pRecipient->SetAddress(pRS->GetStringValue("distributionlistrecipientaddress"));

      pRecipient->SetDelivery(pRS->GetLongValue("distributionlistrecipientdelivery"));
      pRecipient->SetSubscribed(pRS->GetInt64Value("distributionlistrecipientsubscribed"));
      pRecipient->SetConfirmed(pRS->GetLongValue("distributionlistrecipientconfirmed"));
      pRecipient->SetToken(pRS->GetStringValue("distributionlistrecipienttoken"));
      pRecipient->SetHardBounces(pRS->GetLongValue("distributionlistrecipienthardbounces"));
      pRecipient->SetSoftBounces(pRS->GetLongValue("distributionlistrecipientsoftbounces"));
      pRecipient->SetSoftSince(pRS->GetInt64Value("distributionlistrecipientsoftsince"));
      pRecipient->SetLastBounce(pRS->GetInt64Value("distributionlistrecipientlastbounce"));
      pRecipient->SetReason(pRS->GetStringValue("distributionlistrecipientreason"));

      return true;
   }

   bool
   PersistentDistributionListRecipient::SaveObject(std::shared_ptr<DistributionListRecipient> pRecipient)
   {
      String sErrorMessage;

      return SaveObject(pRecipient, sErrorMessage, PersistenceModeNormal);
   }


   bool
   PersistentDistributionListRecipient::SaveObject(std::shared_ptr<DistributionListRecipient> pRecipient, String &sErrorMessage, PersistenceMode mode)
   {
      if (!PreSaveLimitationsCheck::CheckLimitations(mode, pRecipient, sErrorMessage))
         return false;

      SQLStatement oStatement;
      oStatement.SetTable("hm_distributionlistsrecipients");

      oStatement.AddColumnInt64("distributionlistrecipientlistid", pRecipient->GetListID());
      oStatement.AddColumn("distributionlistrecipientaddress", pRecipient->GetAddress());
      oStatement.AddColumn("distributionlistrecipientdelivery", pRecipient->GetDelivery());

      // The rest are the list manager's (ListManager.h) and a new row alone
      // writes them here. On an existing row each is written by the manager's
      // own targeted update below - a counter by adding to it where it is
      // stored - because a whole-row save from an object read before the
      // manager's last write, a COM object in a script or the Control Panel's
      // dialog, would put back a spent token, a bounce count or a state.
      if (pRecipient->GetID() == 0)
      {
         oStatement.AddColumnInt64("distributionlistrecipientsubscribed", pRecipient->GetSubscribed());
         oStatement.AddColumn("distributionlistrecipientconfirmed", pRecipient->GetConfirmed());
         oStatement.AddColumn("distributionlistrecipienttoken", pRecipient->GetToken());
         oStatement.AddColumn("distributionlistrecipienthardbounces", pRecipient->GetHardBounces());
         oStatement.AddColumn("distributionlistrecipientsoftbounces", pRecipient->GetSoftBounces());
         oStatement.AddColumnInt64("distributionlistrecipientsoftsince", pRecipient->GetSoftSince());
         oStatement.AddColumnInt64("distributionlistrecipientlastbounce", pRecipient->GetLastBounce());
         oStatement.AddColumn("distributionlistrecipientreason", pRecipient->GetReason());
      }

      if (pRecipient->GetID() == 0)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("distributionlistrecipientid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String sWhere;
         sWhere.Format(_T("distributionlistrecipientid = %I64d"), pRecipient->GetID());
         oStatement.SetWhereClause(sWhere);
      }

      bool bNewObject = pRecipient->GetID() == 0;

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0);
      if (bRetVal && bNewObject)
         pRecipient->SetID((int) iDBID);

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return bRetVal;
   }

   bool
   PersistentDistributionListRecipient::Reread_(std::shared_ptr<DistributionListRecipient> pRecipient)
   {
      // The row as stored after a targeted write, onto the caller's object.
      std::shared_ptr<DistributionListRecipient> stored(new DistributionListRecipient);
      if (!ReadObject(stored, (long) pRecipient->GetID()))
         return false;

      pRecipient->SetDelivery(stored->GetDelivery());
      pRecipient->SetSubscribed(stored->GetSubscribed());
      pRecipient->SetConfirmed(stored->GetConfirmed());
      pRecipient->SetToken(stored->GetToken());
      pRecipient->SetHardBounces(stored->GetHardBounces());
      pRecipient->SetSoftBounces(stored->GetSoftBounces());
      pRecipient->SetSoftSince(stored->GetSoftSince());
      pRecipient->SetLastBounce(stored->GetLastBounce());
      pRecipient->SetReason(stored->GetReason());
      return true;
   }

   bool
   PersistentDistributionListRecipient::SaveTokenIfEmpty(std::shared_ptr<DistributionListRecipient> pRecipient, const String &token)
   {
      if (pRecipient->GetID() == 0 || token.IsEmpty())
         return false;

      // Written only where there is none yet, so two delivery threads giving
      // the same member a token keep the first, and the object then holds
      // whichever is stored.
      SQLCommand command("update hm_distributionlistsrecipients set distributionlistrecipienttoken = @TOKEN "
                         "where distributionlistrecipientid = @RECIPIENTID and distributionlistrecipienttoken = ''");
      command.AddParameter("@TOKEN", token);
      command.AddParameter("@RECIPIENTID", pRecipient->GetID());

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return Reread_(pRecipient);
   }

   bool
   PersistentDistributionListRecipient::SaveState(std::shared_ptr<DistributionListRecipient> pRecipient)
   {
      if (pRecipient->GetID() == 0)
         return false;

      // A request made, confirmed, approved, or an address that has left:
      // the state, when, the token and the delivery, together.
      SQLCommand command("update hm_distributionlistsrecipients set distributionlistrecipientconfirmed = @CONFIRMED, "
                         "distributionlistrecipientsubscribed = @SUBSCRIBED, distributionlistrecipienttoken = @TOKEN, "
                         "distributionlistrecipientdelivery = @DELIVERY where distributionlistrecipientid = @RECIPIENTID");
      command.AddParameter("@CONFIRMED", pRecipient->GetConfirmed());
      command.AddParameter("@SUBSCRIBED", pRecipient->GetSubscribed());
      command.AddParameter("@TOKEN", pRecipient->GetToken());
      command.AddParameter("@DELIVERY", pRecipient->GetDelivery());
      command.AddParameter("@RECIPIENTID", pRecipient->GetID());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return result;
   }

   bool
   PersistentDistributionListRecipient::SaveDeliveryState(std::shared_ptr<DistributionListRecipient> pRecipient)
   {
      if (pRecipient->GetID() == 0)
         return false;

      // A delivery choice, a reset of the bounces, or the stop the bounces
      // made: the delivery with the counts and the reason that go with it.
      SQLCommand command("update hm_distributionlistsrecipients set distributionlistrecipientdelivery = @DELIVERY, "
                         "distributionlistrecipienthardbounces = @HARD, distributionlistrecipientsoftbounces = @SOFT, "
                         "distributionlistrecipientsoftsince = @SOFTSINCE, distributionlistrecipientreason = @REASON "
                         "where distributionlistrecipientid = @RECIPIENTID");
      command.AddParameter("@DELIVERY", pRecipient->GetDelivery());
      command.AddParameter("@HARD", pRecipient->GetHardBounces());
      command.AddParameter("@SOFT", pRecipient->GetSoftBounces());
      command.AddParameter("@SOFTSINCE", pRecipient->GetSoftSince());
      command.AddParameter("@REASON", pRecipient->GetReason());
      command.AddParameter("@RECIPIENTID", pRecipient->GetID());

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return result;
   }

   bool
   PersistentDistributionListRecipient::AddBounce(std::shared_ptr<DistributionListRecipient> pRecipient, bool hard, __int64 now, __int64 windowStart)
   {
      if (pRecipient->GetID() == 0)
         return false;

      // Counted where the count is stored, so two reports handled at once on
      // two delivery threads both count. A soft bounce inside the window adds
      // to the count; one outside it - or the first - starts a new count at
      // one. The two statements cannot both match one row as it stands.
      bool result = true;

      if (hard)
      {
         SQLCommand add("update hm_distributionlistsrecipients set distributionlistrecipienthardbounces = distributionlistrecipienthardbounces + 1, "
                        "distributionlistrecipientlastbounce = @NOW where distributionlistrecipientid = @RECIPIENTID");
         add.AddParameter("@NOW", now);
         add.AddParameter("@RECIPIENTID", pRecipient->GetID());
         result = Application::Instance()->GetDBManager()->Execute(add);
      }
      else
      {
         SQLCommand add("update hm_distributionlistsrecipients set distributionlistrecipientsoftbounces = distributionlistrecipientsoftbounces + 1, "
                        "distributionlistrecipientlastbounce = @NOW where distributionlistrecipientid = @RECIPIENTID "
                        "and distributionlistrecipientsoftsince <> 0 and distributionlistrecipientsoftsince >= @WINDOWSTART");
         add.AddParameter("@NOW", now);
         add.AddParameter("@RECIPIENTID", pRecipient->GetID());
         add.AddParameter("@WINDOWSTART", windowStart);

         SQLCommand start("update hm_distributionlistsrecipients set distributionlistrecipientsoftbounces = 1, "
                          "distributionlistrecipientsoftsince = @SINCE, distributionlistrecipientlastbounce = @NOW "
                          "where distributionlistrecipientid = @RECIPIENTID "
                          "and (distributionlistrecipientsoftsince = 0 or distributionlistrecipientsoftsince < @WINDOWSTART)");
         start.AddParameter("@SINCE", now);
         start.AddParameter("@NOW", now);
         start.AddParameter("@RECIPIENTID", pRecipient->GetID());
         start.AddParameter("@WINDOWSTART", windowStart);

         result = Application::Instance()->GetDBManager()->Execute(add) &&
                  Application::Instance()->GetDBManager()->Execute(start);
      }

      Cache<DistributionList>::Instance()->RemoveObject(pRecipient->GetListID());

      return result && Reread_(pRecipient);
   }

}