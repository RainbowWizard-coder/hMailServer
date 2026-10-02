// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./PersistentGreyList.h"
#include "../BO/GreyListTriplet.h"
#include "../SQL/DALConnection.h"
#include "../SQL/IPAddressSQLHelper.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentGreyList::PersistentGreyList(void)
   {

   }

   PersistentGreyList::~PersistentGreyList(void)
   {

   }

   std::shared_ptr<GreyListTriplet>
   PersistentGreyList::ReadTriplet_(std::shared_ptr<DALRecordset> pRS)
   {
      std::shared_ptr<GreyListTriplet> pTriplet = std::shared_ptr<GreyListTriplet>(new GreyListTriplet);
      pTriplet->SetID(pRS->GetInt64Value("glid"));
      pTriplet->SetCreateTime(pRS->GetStringValue("glcreatetime"));
      pTriplet->SetBlockEndTime(pRS->GetStringValue("glblockendtime"));
      pTriplet->SetDeleteTime(pRS->GetStringValue("gldeletetime"));

      // An IPv4 row has no second half (IPAddressSQLHelper writes NULL there), and
      // reading that NULL as 0 made the triplet's address an IPv6 one.
      IPAddressSQLHelper helper;
      pTriplet->SetIPAddress(helper.Construct(pRS, "glipaddress1", "glipaddress2"));
      pTriplet->SetSenderAddress(pRS->GetStringValue("glsenderaddress"));
      pTriplet->SetRecipientAddress(pRS->GetStringValue("glrecipientaddress"));
      pTriplet->SetSenderHash(pRS->GetStringValue("glsenderhash"));
      pTriplet->SetRecipientHash(pRS->GetStringValue("glrecipienthash"));

      pTriplet->SetPassedCount(pRS->GetLongValue("glpassedcount"));
      pTriplet->SetBlockedCount(pRS->GetLongValue("glblockedcount"));

      return pTriplet;
   }

   std::shared_ptr<GreyListTriplet>
   PersistentGreyList::GetRecordByKey(const String &senderKey, const String &recipientKey, const IPAddress &remoteIP)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns the grey list triplet with these keys from this address.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<GreyListTriplet> pTriplet;

      if (senderKey.IsEmpty() || recipientKey.IsEmpty())
         return pTriplet;

      IPAddressSQLHelper helper;

      // Ordered by id although the key is unique (schema 6107): should two rows
      // for one triplet ever be there, every connection reads the same one.
      String sSQL;
      sSQL.Format(_T("select * from hm_greylisting_triplets where glipaddress1 %s and glipaddress2 %s and glsenderhash = @SENDERHASH and glrecipienthash = @RECIPIENTHASH order by glid"),
         String(helper.GetAddress1Equals(remoteIP).c_str()),
         String(helper.GetAddress2Equals(remoteIP).c_str()));

      SQLCommand command(sSQL);
      command.AddParameter("@SENDERHASH", senderKey);
      command.AddParameter("@RECIPIENTHASH", recipientKey);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return pTriplet;

      return ReadTriplet_(pRS);
   }

   bool
   PersistentGreyList::AddObject(std::shared_ptr<GreyListTriplet> pTriplet)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Adds a new greylist triple into the database.
   //---------------------------------------------------------------------------()
   {
      if (pTriplet->GetSenderHash().IsEmpty())
         pTriplet->SetSenderHash(GreyListTriplet::ComputeKey(pTriplet->GetSenderAddress()));
      if (pTriplet->GetRecipientHash().IsEmpty())
         pTriplet->SetRecipientHash(GreyListTriplet::ComputeKey(pTriplet->GetRecipientAddress()));

      // Both columns are NOT NULL since schema 6107, and two empty keys written
      // as text would make every triplet from one address the same triplet.
      if (pTriplet->GetSenderHash().IsEmpty() || pTriplet->GetRecipientHash().IsEmpty())
         return false;

      // 320 is the column (schema 6072) and more than RFC 5321's 254. Nothing on
      // the RCPT TO or MAIL FROM line is refused for being longer than that -
      // the only limit on those is 510 octets - so a longer address does reach
      // here, and it is cut: the key is computed over the whole of it, which is
      // what the row is found by, and the column is there for the operator who
      // reads the table.
      String sSenderAddress = pTriplet->GetSenderAddress().Left(320);
      String sRecipientAddress = pTriplet->GetRecipientAddress().Left(320);

      SQLStatement oStatement;

      IPAddressSQLHelper helper;

      oStatement.SetTable("hm_greylisting_triplets");
      oStatement.AddColumn("glcreatetime", pTriplet->GetCreateTime());
      oStatement.AddColumn("glblockendtime", pTriplet->GetBlockEndTime());
      oStatement.AddColumn("gldeletetime", pTriplet->GetDeleteTime());

      helper.AppendStatement(oStatement, pTriplet->GetIPAddress(), "glipaddress1", "glipaddress2");

      oStatement.AddColumn("glsenderaddress", sSenderAddress);
      oStatement.AddColumn("glrecipientaddress", sRecipientAddress);
      oStatement.AddColumn("glpassedcount", "0");
      oStatement.AddColumn("glblockedcount", "0");
      oStatement.AddColumn("glsenderhash", pTriplet->GetSenderHash());
      oStatement.AddColumn("glrecipienthash", pTriplet->GetRecipientHash());

      oStatement.SetStatementType(SQLStatement::STInsert);
      oStatement.SetIdentityColumn("glid");

      // The key is unique (schema 6107), so of two connections presenting the
      // same new triplet at once, both may reach here and one insert is refused.
      // That refusal is the database saying the triplet exists, not a failure:
      // every backend's connection answers it as success when the caller ignores
      // DALErrorInSQL (SQL Server's and Compact's integrity violation, MySQL's
      // duplicate key 1062, PostgreSQL's 23505 alone), so a race costs no
      // error-log line - and the id is what tells the two outcomes apart. Only
      // an insert that wrote a row hands one back; a refused one leaves it at 0
      // (MySQLConnection reads no insert id after a statement whose failure it
      // was told to ignore).
      __int64 insertedId = 0;
      if (!Application::Instance()->GetDBManager()->Execute(oStatement, &insertedId, DALConnection::DALErrorInSQL))
         return false;

      if (insertedId <= 0)
         return false;

      pTriplet->SetID(insertedId);
      return true;
   }

   bool
   PersistentGreyList::ResetDeletionTime(std::shared_ptr<GreyListTriplet> pTriplet)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Resets the deletion time for a triple.
   //---------------------------------------------------------------------------()
   {
      // Update it.
      SQLCommand command("update hm_greylisting_triplets set gldeletetime = @DELETETIME, glpassedcount = glpassedcount+1 where glid = @GLID");
      command.AddParameter("@DELETETIME", pTriplet->GetDeleteTime());
      command.AddParameter("@GLID", pTriplet->GetID());

      bool bResult = Application::Instance()->GetDBManager()->Execute(command);

      return bResult;

   }

   void
   PersistentGreyList::ClearExpiredRecords()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Clears all expired triple records.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command(Formatter::Format("delete from hm_greylisting_triplets where gldeletetime < {0}", SQLStatement::GetCurrentTimestamp()));

      Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   PersistentGreyList::ClearAllRecords()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Clears all triple records.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command("delete from hm_greylisting_triplets");
      Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   PersistentGreyList::IncreaseBlocked(__int64 iTripletID)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Increases the blocked-count for a triple.
   //---------------------------------------------------------------------------()
   {
      SQLCommand command("update hm_greylisting_triplets set glblockedcount = glblockedcount + 1 where glid = @GLID");
      command.AddParameter("@GLID", iTripletID);

      Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentGreyList::Slide(__int64 iTripletID, const String &blockEndTime, const String &deleteTime)
   {
      SQLCommand command("update hm_greylisting_triplets set glblockedcount = glblockedcount + 1, glblockendtime = @BLOCKEND, gldeletetime = @DELETETIME where glid = @GLID");
      command.AddParameter("@BLOCKEND", blockEndTime);
      command.AddParameter("@DELETETIME", deleteTime);
      command.AddParameter("@GLID", iTripletID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentGreyList::Lock(__int64 iTripletID, const String &blockEndTime, const String &deleteTime)
   {
      // The same statement as Slide, kept apart because the two are read apart:
      // a slide moves the window the sender may still walk through, a lock moves
      // the time until which it is refused outright.
      SQLCommand command("update hm_greylisting_triplets set glblockedcount = glblockedcount + 1, glblockendtime = @BLOCKEND, gldeletetime = @DELETETIME where glid = @GLID");
      command.AddParameter("@BLOCKEND", blockEndTime);
      command.AddParameter("@DELETETIME", deleteTime);
      command.AddParameter("@GLID", iTripletID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PersistentGreyList::ResetPending(__int64 iTripletID, const String &blockEndTime, const String &deleteTime)
   {
      SQLCommand command("update hm_greylisting_triplets set glblockedcount = 0, glpassedcount = 0, glblockendtime = @BLOCKEND, gldeletetime = @DELETETIME where glid = @GLID");
      command.AddParameter("@BLOCKEND", blockEndTime);
      command.AddParameter("@DELETETIME", deleteTime);
      command.AddParameter("@GLID", iTripletID);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

}
