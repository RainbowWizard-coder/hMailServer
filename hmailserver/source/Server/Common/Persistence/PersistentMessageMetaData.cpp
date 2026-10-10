// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./PersistentMessageMetaData.h"
#include "./PersistentMessage.h"
#include "../BO/Message.h"

#include "../SQL/SQLStatement.h"
#include "../Util/Time.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PersistentMessageMetaData::PersistentMessageMetaData(void)
   {
   }

   PersistentMessageMetaData::~PersistentMessageMetaData(void)
   {
   }

   // 2011-11-15 JDR: Modified this function to include quickIndex to pick a specific query.
   std::set<std::shared_ptr<PersistentMessageMetaData::MessageInfo> >
   PersistentMessageMetaData::GetMessagesToIndex(bool quickIndex = false)
   {
      PersistentMessage persistentMessage;
      std::set<std::shared_ptr<MessageInfo> > result;

      SQLStatement statement;
      statement.AddColumn("messageid");
      statement.AddColumn("messageaccountid");
      statement.AddColumn("messagefolderid");
      statement.AddColumn("messagefilename");
      statement.AddColumn("accountaddress");
      statement.SetStatementType(SQLStatement::STSelect);
      statement.SetTable("hm_messages");

      // Are we doing a quick index? Quick means that we only index recent messages and ignore older ones.
      if (quickIndex == true) 
      {
         // 1000 default
         int iIndexerQuickLimit = IniFileSettings::Instance()->GetIndexerQuickLimit();
         int latestMessageId = persistentMessage.GetLatestMessageId();
         int oldestMessageToIndex = latestMessageId - iIndexerQuickLimit;
         if (oldestMessageToIndex < 0)
            oldestMessageToIndex = 0;

         // Only pick the last iIndexerQuickLimit records... very quick compared to full
         //
         // Never a message of a mailbox encrypted at rest (Util/MailboxKeys.h):
         // the sort's copy of its From, To, Cc and Subject would be those
         // headers in the clear beside a file that hides them. SORT reads such
         // a message's header from the message, while its key is open.
         String whereClause;
         whereClause.Format(_T("hm_messages.messagetype = 2 AND hm_messages.messageid > %d AND hm_message_metadata.metadata_id IS NULL AND hm_messages.messageaccountid NOT IN (SELECT akaccountid FROM hm_accountkeys)"), oldestMessageToIndex);
         statement.SetWhereClause(whereClause);
         statement.SetAdditionalSQL("LEFT JOIN hm_accounts ON hm_messages.messageaccountid = hm_accounts.accountid LEFT JOIN hm_message_metadata ON hm_messages.messageid = hm_message_metadata.metadata_messageid ");
      } 
      else 
      {
         // 25000 default
         int iIndexerFullLimit = IniFileSettings::Instance()->GetIndexerFullLimit();

         // Otherwise do the old/standard way (pick iIndexerFullLimit records max)
         statement.SetWhereClause("messagetype = 2 AND NOT EXISTS (SELECT metadata_messageid FROM hm_message_metadata WHERE hm_messages.messagetype = 2 and hm_messages.messageid = hm_message_metadata.metadata_messageid) AND hm_messages.messageaccountid NOT IN (SELECT akaccountid FROM hm_accountkeys)");
         statement.SetAdditionalSQL("left join hm_accounts on hm_messages.messageaccountid = hm_accounts.accountid");
         statement.SetTopRows(iIndexerFullLimit);
      }

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(statement);

      if (!pRS)
         return result;

      while (!pRS->IsEOF())
      {
         std::shared_ptr<MessageInfo> messageInfo = std::shared_ptr<MessageInfo>(new MessageInfo);
         messageInfo->MessageID = pRS->GetInt64Value("messageid");
         messageInfo->AccountID = pRS->GetLongValue("messageaccountid");
         messageInfo->FolderID = pRS->GetLongValue("messagefolderid");
         
         String accountAddress = pRS->GetStringValue("accountaddress");
         String fileName = pRS->GetStringValue("messagefilename");

         std::shared_ptr<Message> dummyMessage = std::shared_ptr<Message>(new Message);
         dummyMessage->SetID(messageInfo->MessageID);
         dummyMessage->SetAccountID(messageInfo->AccountID);
         dummyMessage->SetFolderID(messageInfo->FolderID);
         dummyMessage->SetPartialFileName(fileName);

         String fullPath = PersistentMessage::GetFileName(accountAddress, dummyMessage);
         
         messageInfo->FileName = fullPath;

         result.insert(messageInfo);
         pRS->MoveNext();
      }

      return result;
   }

   void
   PersistentMessageMetaData::GetMetaData(int accountID, int folderID, const String &headerField, std::map<__int64, String > &result)
   {
      MessageMetaData::MetaDataField field = MessageMetaData::GetMetaDataField(headerField);
      
      SQLStatement sql;
      sql.SetStatementType(SQLStatement::STSelect);
      sql.SetTable("hm_message_metadata");
      sql.AddColumn("metadata_messageid");

      switch (field)
      {
      case MessageMetaData::From:
         sql.AddColumn("metadata_from");
         break;
      case MessageMetaData::Date:
         sql.AddColumn("metadata_dateutc");
         break;
      case MessageMetaData::Subject:
         sql.AddColumn("metadata_subject");
         break;
      case MessageMetaData::To:
         sql.AddColumn("metadata_to");
         break;
      case MessageMetaData::CC:
         sql.AddColumn("metadata_cc");
         break;
      }
      
      String whereClause;
      whereClause.Format(_T("metadata_accountid = %d and metadata_folderid = %d"), accountID, folderID);
      sql.SetWhereClause(whereClause);

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(sql);

      while (!pRS->IsEOF())
      {
         String value;
         __int64 messageID = pRS->GetInt64Value("metadata_messageid");     

         switch (field)
         {
         case MessageMetaData::From:
            value = pRS->GetStringValue("metadata_from");
            break;
         case MessageMetaData::Date:
            value = pRS->GetStringValue("metadata_dateutc");
            break;
         case MessageMetaData::Subject:
            value = pRS->GetStringValue("metadata_subject");
            break;
         case MessageMetaData::To:
            value = pRS->GetStringValue("metadata_to");
            break;
         case MessageMetaData::CC:
            value = pRS->GetStringValue("metadata_cc");
            break;
         }

         result.insert(std::make_pair(messageID, value));

         pRS->MoveNext();
      }

   }


   bool 
   PersistentMessageMetaData::SaveObject(std::shared_ptr<MessageMetaData> metaData)
   {
      SQLStatement statement;
      statement.SetTable("hm_message_metadata");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("metadata_id");
      statement.AddColumn("metadata_accountid", metaData->GetAccountID());
      statement.AddColumn("metadata_folderid", metaData->GetFolderID());
      statement.AddColumnInt64("metadata_messageid", metaData->GetMessageID());
      statement.AddColumnDate("metadata_dateutc", metaData->GetDate());
      statement.AddColumn("metadata_from", metaData->GetFrom(), 100);
      statement.AddColumn("metadata_subject", metaData->GetSubject(), 100);
      statement.AddColumn("metadata_to", metaData->GetTo(), 100);
      statement.AddColumn("metadata_cc", metaData->GetCC(), 100);

      // Save and fetch ID
      __int64 iDBID = 0;
      bool bRetVal = Application::Instance()->GetDBManager()->Execute(statement, &iDBID);
      metaData->SetID((int) iDBID);

      return bRetVal;
   }

   bool 
   PersistentMessageMetaData::DeleteOrphanedItems()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Deletes the metadata of messages no longer in the system, a thousand
   // message ids at a time, from the lowest the table names upwards, each slice
   // its own statement and a short pause between two - as
   // PersistentMessageIndex::DeleteOrphanedItems has swept the full-text terms
   // since 9c76455c1. It was one statement over the whole table, which the
   // message indexer ran at every start before anything else, with no
   // interruption point in it: a stop of the servers then waited for the whole
   // of it (MessageIndexer::Stop), and on SQL Server Compact the scheduled tasks
   // that start with the service waited for the database's one connection as
   // they did for the terms' sweep. Now a stop waits for a slice, and the
   // indexer sweeps once it has first caught up after a start, then on the
   // full-index cadence (MessageIndexer::WorkerFuncInternal). The gaps between
   // the ids the table names are stepped over, not walked.
   //---------------------------------------------------------------------------()
   {
      const __int64 sliceIds = 1000;

      __int64 from = 0;
      for (;;)
      {
         SQLCommand lowest("select min(metadata_messageid) as low from hm_message_metadata where metadata_messageid >= @FROM");
         lowest.AddParameter("@FROM", from);

         std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(lowest);
         if (!pRS)
            return false;

         if (pRS->IsEOF() || pRS->GetIsNull("low"))
            return true;

         const __int64 low = pRS->GetInt64Value("low");
         pRS.reset();

         SQLCommand command("delete from hm_message_metadata where metadata_messageid >= @LOW and metadata_messageid < @HIGH "
                            "and not exists (select messageid from hm_messages where messageid = metadata_messageid)");
         command.AddParameter("@LOW", low);
         command.AddParameter("@HIGH", low + sliceIds);

         if (!Application::Instance()->GetDBManager()->Execute(command))
            return false;

         from = low + sliceIds;

         // Housekeeping, so the rest of the server goes first; also where a
         // stop of the servers ends the sweep.
         boost::this_thread::sleep_for(boost::chrono::milliseconds(50));
      }
   }

   bool
   PersistentMessageMetaData::DeleteForMessage(std::shared_ptr<Message> message)
   {
      if (message->GetState() != Message::Delivered)
      {
         // We don't store metadata for undelivered messages.
         return true;
      }
         
      SQLCommand command ("delete from hm_message_metadata where metadata_messageid = @MESSAGEID");
      command.AddParameter("@MESSAGEID", message->GetID());

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   int
   PersistentMessageMetaData::GetTotalMessageCount()
   {
      SQLCommand command("select count(*) as c from hm_message_metadata");

      std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!pRS)
         return false;

      int result = pRS->GetLongValue("c");;

      return result;

   }

   void
   PersistentMessageMetaData::Clear()
   {
      SQLStatement statement (SQLStatement::STDelete, "hm_message_metadata");
 
      Application::Instance()->GetDBManager()->Execute(statement);

   }

}