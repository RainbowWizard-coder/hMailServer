// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"

#include "PersistentDistributionList.h"

#include "../Cache/Cache.h"

#include "PersistentDistributionListRecipient.h"
#include "PreSaveLimitationsCheck.h"

#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionList.h"
#include "../Util/ListArchive.h"
#include "../Util/ListManager.h"



#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentDistributionList::PersistentDistributionList(void)
   {
   
   }

   PersistentDistributionList::~PersistentDistributionList(void)
   {

   }

   bool
   PersistentDistributionList::DeleteObject(std::shared_ptr<DistributionList> pDistList)
   {
      if (pDistList->GetID() == 0)
      {
         HM_ASSERT(0);
         return false;
      }

      // Unchecked, and the list row below is deleted regardless - so a failure here
      // leaves every member row behind, pointing at a distribution list that no longer
      // exists. Nothing reads those rows again and nothing cleans them up, and they are
      // addresses: on a server where lists are created and removed by a provisioning
      // script, that is an unbounded accumulation of somebody's recipients.
      //
      // Reported rather than fatal, for the same reason as the account cascade: this is
      // a delete with no transaction around it, and stopping half way leaves more behind
      // rather than less.
      if (!DeleteMembers(pDistList))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6116, "PersistentDistributionList::DeleteObject",
            Formatter::Format("The members of distribution list {0} could not be deleted and have been left behind as orphan rows.",
               pDistList->GetAddress()));
      }

      // The list manager's archive goes with the list: its rows (which the
      // foreign key would take anyway) and its files under the data
      // directory's Lists folder, which nothing else would ever remove.
      ListArchive::DeleteAll(pDistList->GetID());

      SQLCommand deleteCommand("delete from hm_distributionlists where distributionlistid = @LISTID");
      deleteCommand.AddParameter("@LISTID", pDistList->GetID());

      bool bResult = Application::Instance()->GetDBManager()->Execute(deleteCommand);
   
      Cache<DistributionList>::Instance()->RemoveObject(pDistList);

      // The delivery path asks whether any list is managed at all; this may
      // have been the last one.
      ListManager::ForgetManagedState();

      return bResult;
   }

   bool
   PersistentDistributionList::ReadObject(std::shared_ptr<DistributionList> pDistList, const String &sAddress)
   {
      SQLStatement statement;

      statement.SetStatementType(SQLStatement::STSelect);
      statement.SetTable("hm_distributionlists");
      statement.AddWhereClauseColumn("distributionlistaddress", sAddress);

      return ReadObject(pDistList, statement.GetCommand());
   }  

   bool
   PersistentDistributionList::ReadObject(std::shared_ptr<DistributionList> pDistList, __int64 iID)
   {
      SQLCommand selectCommand("select * from hm_distributionlists where distributionlistid = @LISTID");
      selectCommand.AddParameter("@LISTID", iID);
  
      return ReadObject(pDistList, selectCommand);
   }

   bool
   PersistentDistributionList::ReadObject(std::shared_ptr<DistributionList> pDistList, const SQLCommand &command)
   {
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return false;

      bool bRetVal = false;
      if (!pRS->IsEOF())
      {
         bRetVal = ReadObject(pDistList, pRS);
      }

   
      return bRetVal;
   }



   bool
   PersistentDistributionList::ReadObject(std::shared_ptr<DistributionList> pDistList, std::shared_ptr<DALRecordset> pRS)
   {
      pDistList->SetID(pRS->GetLongValue("distributionlistid"));
      pDistList->SetDomainID(pRS->GetLongValue("distributionlistdomainid"));
      pDistList->SetActive(pRS->GetLongValue("distributionlistenabled") ? true : false);
      pDistList->SetAddress(pRS->GetStringValue("distributionlistaddress"));
      pDistList->SetRequireAuth(pRS->GetLongValue("distributionlistrequireauth") ? true : false);
      pDistList->SetRequireAddress(pRS->GetStringValue("distributionlistrequireaddress"));
      pDistList->SetListMode((HM::DistributionList::ListMode) pRS->GetLongValue("distributionlistmode"));
      pDistList->SetModeratorAddress(pRS->GetStringValue("distributionlistmoderatoraddress"));
      pDistList->SetBounceAddress(pRS->GetStringValue("distributionlistbounceaddress"));
      pDistList->SetMembership(pRS->GetStringValue("distributionlistmembership"));

      pDistList->SetManaged(pRS->GetLongValue("distributionlistmanaged") ? true : false);
      pDistList->SetSubscribePolicy(pRS->GetLongValue("distributionlistsubscribe"));
      pDistList->SetArchive(pRS->GetLongValue("distributionlistarchive"));
      pDistList->SetArchiveDays(pRS->GetLongValue("distributionlistarchivedays"));
      pDistList->SetDigest(pRS->GetLongValue("distributionlistdigest") ? true : false);
      pDistList->SetDigestHour(pRS->GetLongValue("distributionlistdigesthour"));
      pDistList->SetDigestLastId(pRS->GetInt64Value("distributionlistdigestlastid"));
      pDistList->SetDigestLastTime(pRS->GetInt64Value("distributionlistdigestlasttime"));
      pDistList->SetReplyToList(pRS->GetLongValue("distributionlistreplyto") ? true : false);
      pDistList->SetSubjectPrefix(pRS->GetStringValue("distributionlistprefix"));
      pDistList->SetFooter(pRS->GetStringValue("distributionlistfooter"));
      pDistList->SetVerp(pRS->GetLongValue("distributionlistverp") ? true : false);
      pDistList->SetOwner(pRS->GetStringValue("distributionlistowner"));
      pDistList->SetSecret(pRS->GetStringValue("distributionlistsecret"));

      return true;
   }

   bool
   PersistentDistributionList::SaveObject(std::shared_ptr<DistributionList> pDistList)
   {
      String sErrorMessage;
      return SaveObject(pDistList, sErrorMessage, PersistenceModeNormal);
   }

   bool
   PersistentDistributionList::SaveObject(std::shared_ptr<DistributionList> pDistList, String &sErrorMessage, PersistenceMode mode)
   {
      if (!PreSaveLimitationsCheck::CheckLimitations(mode, pDistList, sErrorMessage))
         return false;

      SQLStatement oStatement;
      oStatement.SetTable("hm_distributionlists");

      oStatement.AddColumnInt64("distributionlistdomainid", pDistList->GetDomainID());
      oStatement.AddColumn("distributionlistenabled", pDistList->GetActive() ? 1 : 0);
      oStatement.AddColumn("distributionlistaddress", pDistList->GetAddress());
      oStatement.AddColumn("distributionlistrequireauth", pDistList->GetRequireAuth() ? 1 : 0);
      oStatement.AddColumn("distributionlistrequireaddress", pDistList->GetRequireAddress());
      oStatement.AddColumn("distributionlistmode", pDistList->GetListMode());
      oStatement.AddColumn("distributionlistmoderatoraddress", pDistList->GetModeratorAddress());
      oStatement.AddColumn("distributionlistbounceaddress", pDistList->GetBounceAddress());
      oStatement.AddColumn("distributionlistmembership", pDistList->GetMembership());

      oStatement.AddColumn("distributionlistmanaged", pDistList->GetManaged() ? 1 : 0);
      oStatement.AddColumn("distributionlistsubscribe", pDistList->GetSubscribePolicy());
      oStatement.AddColumn("distributionlistarchive", pDistList->GetArchive());
      oStatement.AddColumn("distributionlistarchivedays", pDistList->GetArchiveDays());
      oStatement.AddColumn("distributionlistdigest", pDistList->GetDigest() ? 1 : 0);
      oStatement.AddColumn("distributionlistdigesthour", pDistList->GetDigestHour());
      oStatement.AddColumn("distributionlistreplyto", pDistList->GetReplyToList() ? 1 : 0);
      oStatement.AddColumn("distributionlistprefix", pDistList->GetSubjectPrefix());
      oStatement.AddColumn("distributionlistfooter", pDistList->GetFooter());
      oStatement.AddColumn("distributionlistverp", pDistList->GetVerp() ? 1 : 0);
      oStatement.AddColumn("distributionlistowner", pDistList->GetOwner());

      // Where the last digest stopped, when it went, and the list's secret are
      // the manager's own (SaveDigestState, SaveSecretIfEmpty), and are written
      // by a new list alone: a Control Panel dialog or a COM object opened
      // before the day's digest, saved after it, would otherwise put the old
      // position back and the next pass would send the day again.
      if (pDistList->GetID() == 0)
      {
         oStatement.AddColumnInt64("distributionlistdigestlastid", pDistList->GetDigestLastId());
         oStatement.AddColumnInt64("distributionlistdigestlasttime", pDistList->GetDigestLastTime());
         oStatement.AddColumn("distributionlistsecret", pDistList->GetSecret());
      }

      // A digest switched on starts from the newest message the archive holds,
      // not from the oldest: a list that kept an archive for months before
      // its digest was switched on - or had it switched off for a while, when
      // its digest members were sent each message - would otherwise send them
      // all again, a hundred a day. Asked of the row as stored before this
      // save writes it, so only the save that switches it on moves it.
      bool digestSwitchedOn = false;
      if (pDistList->GetID() != 0 && pDistList->GetDigest())
      {
         SQLCommand stored("select distributionlistdigest from hm_distributionlists where distributionlistid = @LISTID");
         stored.AddParameter("@LISTID", pDistList->GetID());

         std::shared_ptr<DALRecordset> storedRS = Application::Instance()->GetDBManager()->OpenRecordset(stored);
         digestSwitchedOn = storedRS && !storedRS->IsEOF() && storedRS->GetLongValue("distributionlistdigest") == 0;
      }

      if (pDistList->GetID() == 0)
      {
         oStatement.SetStatementType(SQLStatement::STInsert);
         oStatement.SetIdentityColumn("distributionlistid");
      }
      else
      {
         oStatement.SetStatementType(SQLStatement::STUpdate);

         String sWhere;
         sWhere.Format(_T("distributionlistid = %I64d"), pDistList->GetID());
         oStatement.SetWhereClause(sWhere);
      }

      bool bNewObject = pDistList->GetID() == 0;

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(oStatement, bNewObject ? &iDBID : 0);
      if (bRetVal && bNewObject)
         pDistList->SetID((int) iDBID);

      if (bRetVal && digestSwitchedOn)
      {
         __int64 newest = 0;

         SQLCommand highest("select max(laid) as highest from hm_listarchive where lalistid = @LISTID");
         highest.AddParameter("@LISTID", pDistList->GetID());

         // A max() over no rows is one row holding NULL: an empty archive, and
         // the position stays at 0.
         std::shared_ptr<DALRecordset> highestRS = Application::Instance()->GetDBManager()->OpenRecordset(highest);
         if (highestRS && !highestRS->IsEOF() && !highestRS->GetIsNull("highest"))
            newest = highestRS->GetInt64Value("highest");

         SQLCommand position("update hm_distributionlists set distributionlistdigestlastid = @LASTID where distributionlistid = @LISTID");
         position.AddParameter("@LASTID", newest);
         position.AddParameter("@LISTID", pDistList->GetID());

         if (Application::Instance()->GetDBManager()->Execute(position))
            pDistList->SetDigestLastId(newest);
      }

      Cache<DistributionList>::Instance()->RemoveObject(pDistList);

      // A list switched to managed, or back, is seen by the next delivery
      // rather than a minute later.
      ListManager::ForgetManagedState();

      return bRetVal;
   }
   
   bool
   PersistentDistributionList::DeleteMembers(std::shared_ptr<DistributionList> pDistList)
   {
      return PersistentDistributionListRecipient::DeleteByListID(pDistList->GetID());     
   }

   bool
   PersistentDistributionList::SaveDigestState(__int64 listId, __int64 lastArchiveId, __int64 sentTime)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Where the list's last digest stopped, and when it went. Two columns the
   // digest alone writes, so they are written alone: a whole-list save here
   // would put back whatever an administrator changed while the digest was
   // being built.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command("update hm_distributionlists set distributionlistdigestlastid = @LASTID, distributionlistdigestlasttime = @SENT where distributionlistid = @LISTID");
      command.AddParameter("@LASTID", lastArchiveId);
      command.AddParameter("@SENT", sentTime);
      command.AddParameter("@LISTID", listId);

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      Cache<DistributionList>::Instance()->RemoveObject(listId);

      return result;
   }

   bool
   PersistentDistributionList::SaveSecretIfEmpty(__int64 listId, const String &secret)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The list's secret, written only where the column is still empty, so two
   // threads making one each keep the first: the caller reads the list again
   // and uses whatever is stored.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command("update hm_distributionlists set distributionlistsecret = @SECRET where distributionlistid = @LISTID and distributionlistsecret = ''");
      command.AddParameter("@SECRET", secret);
      command.AddParameter("@LISTID", listId);

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      Cache<DistributionList>::Instance()->RemoveObject(listId);

      return result;
   }


}