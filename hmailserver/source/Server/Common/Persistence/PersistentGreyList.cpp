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
   std::atomic<bool> PersistentGreyList::legacy_rows_may_remain_(true);
   std::atomic<bool> PersistentGreyList::legacy_rows_counted_(false);

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

      pTriplet->SetIPAddress(IPAddress(pRS->GetInt64Value("glipaddress1"), pRS->GetInt64Value("glipaddress2")));
      pTriplet->SetSenderAddress(pRS->GetStringValue("glsenderaddress"));
      pTriplet->SetRecipientAddress(pRS->GetStringValue("glrecipientaddress"));

      if (!pRS->GetIsNull("glsenderhash"))
         pTriplet->SetSenderHash(pRS->GetStringValue("glsenderhash"));
      if (!pRS->GetIsNull("glrecipienthash"))
         pTriplet->SetRecipientHash(pRS->GetStringValue("glrecipienthash"));

      pTriplet->SetPassedCount(pRS->GetLongValue("glpassedcount"));
      pTriplet->SetBlockedCount(pRS->GetLongValue("glblockedcount"));

      return pTriplet;
   }

   std::shared_ptr<GreyListTriplet>
   PersistentGreyList::GetRecord(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress & remoteIP)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns a grey list triple based on sender, recipient and IP address.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<GreyListTriplet> pTriplet;

      const String senderKey = GreyListTriplet::ComputeKey(sSenderAddress);
      const String recipientKey = GreyListTriplet::ComputeKey(sRecipientAddress);

      if (senderKey.IsEmpty() || recipientKey.IsEmpty())
      {
         // The digest could not be computed, which a stock OpenSSL never refuses.
         // Said once, and the triplet is then matched the way it was before the
         // key existed: by the address columns, on rows written without a key.
         static std::atomic<bool> reported(false);
         if (!reported.exchange(true))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 6950, "PersistentGreyList::GetRecord",
               "The greylisting key could not be computed - this build's OpenSSL answered no MD5 digest. "
               "Triplets are matched by their addresses, as they were before schema 6072, until it does.");
         }

         return FindLegacy_(sSenderAddress, sRecipientAddress, remoteIP, String(), String());
      }

      IPAddressSQLHelper helper;

      String sSQL;
      sSQL.Format(_T("select * from hm_greylisting_triplets where glipaddress1 %s and glipaddress2 %s and glsenderhash = @SENDERHASH and glrecipienthash = @RECIPIENTHASH"),
         String(helper.GetAddress1Equals(remoteIP).c_str()),
         String(helper.GetAddress2Equals(remoteIP).c_str()));

      SQLCommand command(sSQL);
      command.AddParameter("@SENDERHASH", senderKey);
      command.AddParameter("@RECIPIENTHASH", recipientKey);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return pTriplet;

      if (!pRS->IsEOF())
         return ReadTriplet_(pRS);

      // Not found by the key. A row written before schema 6072 has no key, and is
      // found by its addresses as they were written then; it leaves here with its
      // key, so this is the one time it is looked for this way.
      if (LegacyRowsMayRemain_())
         return FindLegacy_(sSenderAddress, sRecipientAddress, remoteIP, senderKey, recipientKey);

      return pTriplet;
   }

   std::shared_ptr<GreyListTriplet>
   PersistentGreyList::FindLegacy_(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP, const String &senderKey, const String &recipientKey)
   {
      std::shared_ptr<GreyListTriplet> pTriplet;

      IPAddressSQLHelper helper;

      // The 200 characters are what AddObject wrote until schema 6072 - the
      // truncation the key exists to end - so a legacy row is matched by exactly
      // the value it holds.
      String sSQL;
      sSQL.Format(_T("select * from hm_greylisting_triplets where glipaddress1 %s and glipaddress2 %s and glsenderhash is null and glsenderaddress = @SENDERADDRESS and glrecipientaddress = @RECIPIENTADDRESS"),
         String(helper.GetAddress1Equals(remoteIP).c_str()),
         String(helper.GetAddress2Equals(remoteIP).c_str()));

      SQLCommand command(sSQL);
      command.AddParameter("@SENDERADDRESS", sSenderAddress.Left(200));
      command.AddParameter("@RECIPIENTADDRESS", sRecipientAddress.Left(200));

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return pTriplet;

      pTriplet = ReadTriplet_(pRS);

      if (senderKey.IsEmpty() || recipientKey.IsEmpty())
         return pTriplet;

      // Give the row its key and the whole of the addresses, so that from now on
      // it is found by the key and a second address sharing the first 200
      // characters is a triplet of its own. A failure here costs nothing but a
      // repeat of this lookup next time.
      SQLCommand rewrite("update hm_greylisting_triplets set glsenderhash = @SENDERHASH, glrecipienthash = @RECIPIENTHASH, glsenderaddress = @SENDERADDRESS, glrecipientaddress = @RECIPIENTADDRESS where glid = @GLID");
      rewrite.AddParameter("@SENDERHASH", senderKey);
      rewrite.AddParameter("@RECIPIENTHASH", recipientKey);
      rewrite.AddParameter("@SENDERADDRESS", sSenderAddress.Left(320));
      rewrite.AddParameter("@RECIPIENTADDRESS", sRecipientAddress.Left(320));
      rewrite.AddParameter("@GLID", pTriplet->GetID());

      if (Application::Instance()->GetDBManager()->Execute(rewrite))
      {
         pTriplet->SetSenderHash(senderKey);
         pTriplet->SetRecipientHash(recipientKey);
         pTriplet->SetSenderAddress(sSenderAddress.Left(320));
         pTriplet->SetRecipientAddress(sRecipientAddress.Left(320));
      }

      return pTriplet;
   }

   bool
   PersistentGreyList::LegacyRowsMayRemain_()
   {
      if (!legacy_rows_may_remain_)
         return false;

      if (!legacy_rows_counted_.exchange(true))
      {
         const int anyLegacy = AnyLegacyRow();
         if (anyLegacy == 0)
            legacy_rows_may_remain_ = false;
         else if (anyLegacy < 0)
            legacy_rows_counted_ = false;
      }

      return legacy_rows_may_remain_;
   }

   int
   PersistentGreyList::AnyLegacyRow()
   {
      // The question is "is there one?", and it is asked again after every
      // sweep until the answer is no - on an upgraded busy server, for as long
      // as GreyListingFinalDelete, which is 36 days by default. count(*) over
      // the whole triplet table answers a larger question than that at a cost
      // that grows with the table, so this asks for a single value instead.
      //
      // min() rather than a plain select of the ids: a scalar aggregate answers
      // exactly one row, NULL where nothing matched, on every backend, while a
      // plain select hands back every matching row and the PostgreSQL and MySQL
      // recordsets here buffer all of them - which on the server this is about
      // would be the whole of the un-migrated table, every sweep. (And neither
      // `case when exists`, which crashes the SQL Server Compact provider, nor
      // `where glid = (select max(glid))`, which it refuses outright.)
      SQLCommand command("select min(glid) as glid from hm_greylisting_triplets where glsenderhash is null");

      // -1 when the question could not be asked, which is not 0: a failed
      // question must not end the legacy lookup for good.
      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS || pRS->IsEOF())
         return -1;

      return pRS->GetIsNull("glid") ? 0 : 1;
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

      const bool haveKeys = !pTriplet->GetSenderHash().IsEmpty() && !pTriplet->GetRecipientHash().IsEmpty();

      // 320 is the column (schema 6072) and more than RFC 5321's 254. Nothing on
      // the RCPT TO or MAIL FROM line is refused for being longer than that -
      // the only limit on those is 510 octets - so a longer address does reach
      // here, and it is cut: the key is computed over the whole of it, which is
      // what the row is found by, and the column is there for the operator who
      // reads the table.
      //
      // WITH NO KEY, 200. A row written without one is found again only by
      // FindLegacy_, which matches the first 200 characters, so writing 320 of
      // them would write a row that the next lookup could never match: a new row
      // every attempt, that sender deferred for ever, and the table growing
      // without bound. That is the case this fallback exists for - the addresses
      // longer than 200 characters are exactly the ones discussion #281 is about
      // - so the write matches the read.
      const int width = haveKeys ? 320 : 200;

      String sSenderAddress = pTriplet->GetSenderAddress().Left(width);
      String sRecipientAddress = pTriplet->GetRecipientAddress().Left(width);

      // Update the triplet
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

      // A key that could not be computed is left NULL, which is the legacy row's
      // shape: GetRecord finds such a row by its addresses. Two empty keys written
      // as text would make every triplet from one address the same triplet.
      if (haveKeys)
      {
         oStatement.AddColumn("glsenderhash", pTriplet->GetSenderHash());
         oStatement.AddColumn("glrecipienthash", pTriplet->GetRecipientHash());
      }

      oStatement.SetStatementType(SQLStatement::STInsert);
      oStatement.SetIdentityColumn("glid");

      bool bResult = Application::Instance()->GetDBManager()->Execute(oStatement, 0, DALConnection::DALErrorInSQL);

      return bResult;
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

      // The sweep is what removes the last row without a key - a legacy row that
      // was never matched again expires as any pending or confirmed row does - so
      // this is the moment to ask whether any remain. Once none do, none ever
      // will, and the lookup stops asking.
      if (legacy_rows_may_remain_ && AnyLegacyRow() == 0)
         legacy_rows_may_remain_ = false;
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

      legacy_rows_may_remain_ = false;
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
