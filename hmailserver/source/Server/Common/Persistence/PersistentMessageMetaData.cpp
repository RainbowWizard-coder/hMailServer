// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./PersistentMessageMetaData.h"
#include "./PersistentMessage.h"
#include "../BO/Message.h"

#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseSettings.h"
#include "../Application/IniFileSettings.h"
#include "../Util/Time.h"

#include <stdexcept>

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
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Writes the sort's copy of a message's headers, the row the message indexer
   // makes once for each delivered message, as an INSERT ... SELECT from
   // hm_messages of the message's own row - its id, account and folder - as
   // PersistentMessageIndex::SaveTermForMessage writes a term of the full-text
   // index. A message that is deleted between the indexer's query and this
   // write - a message expunged or a mailbox emptied while the indexer catches
   // up, the regression suite's setup clearing the last test's mail at the start
   // of the next while the indexer indexes it - then inserts nothing and
   // succeeds, which is the truth of it: the row of a message that is gone is
   // not wanted, and the indexer's next pass does not ask for it. The plain
   // INSERT of the values this was, in every version to 6.3.6, was refused for
   // such a message by the foreign key hm_message_metadata has had on
   // hm_messages since schema 6030 - "Value violated the integrity constraints"
   // on SQL Server Compact - which DALConnection::Execute reported at High as
   // HM5032, and the indexer gave the batch up (MessageIndexer::IndexMessages_).
   // Seen twice in batch48's windows-gate-s1vm run of 17 October 2026, both at a
   // test's setup. The values are literals, the header fields escaped and
   // N'...' on SQL Server and Compact as the term is, since SQL Server Compact
   // allows no parameter in a select list, and each of the four fields is cut to
   // a hundred characters, as the INSERT cut them; a date older than 1800, or
   // none, is NULL, as SQLStatement::AddColumnDate makes it. The account and
   // folder are read from the message's row, the ones the indexer's query read.
   // Nothing reads the row's id after the write, which the INSERT fetched.
   //---------------------------------------------------------------------------()
   {
      const DatabaseSettings::SQLDBType databaseType = IniFileSettings::Instance()->GetDatabaseType();
      const bool unicodeLiteral = databaseType == DatabaseSettings::TypeMSSQLServer ||
                                  databaseType == DatabaseSettings::TypeMSSQLCompactEdition;

      const auto headerLiteral = [unicodeLiteral](const String &value) -> String
      {
         String cut = value;

         if (cut.GetLength() > 100)
            cut = cut.Mid(0, 100);

         return String(unicodeLiteral ? _T("N'") : _T("'")) + SQLStatement::Escape(cut) + _T("'");
      };

      const DateTime date = metaData->GetDate();
      String dateLiteral;

      if (date.GetStatus() == DateTime::invalid || date.GetYear() < 1800)
         dateLiteral = _T("NULL");
      else
         dateLiteral = _T("'") + Time::GetTimeStampFromDateTime(date) + _T("'");

      String sql = _T("insert into hm_message_metadata (metadata_accountid, metadata_folderid, metadata_messageid, metadata_dateutc, metadata_from, metadata_subject, metadata_to, metadata_cc) ");
      sql += _T("select messageaccountid, messagefolderid, messageid, ");
      sql += dateLiteral + _T(", ");
      sql += headerLiteral(metaData->GetFrom()) + _T(", ");
      sql += headerLiteral(metaData->GetSubject()) + _T(", ");
      sql += headerLiteral(metaData->GetTo()) + _T(", ");
      sql += headerLiteral(metaData->GetCC());
      sql += _T(" from hm_messages where messageid = @MESSAGEID");

      SQLCommand command(sql);
      command.AddParameter("@MESSAGEID", metaData->GetMessageID());

      return Application::Instance()->GetDBManager()->Execute(command);
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

   namespace
   {
      void MetaDataFailed(const char *why)
      {
         throw std::logic_error(std::string("MessageMetaDataTester: ") + why);
      }

      // The rows of hm_message_metadata for one message id.
      __int64 MetaDataRowsFor(__int64 messageId)
      {
         SQLCommand command(_T("select count(*) as c from hm_message_metadata where metadata_messageid = @MESSAGEID"));
         command.AddParameter("@MESSAGEID", messageId);

         std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!pRS)
            MetaDataFailed("the rows of the sort's copy for a message could not be counted.");

         return pRS->GetInt64Value("c");
      }
   }

   void
   MessageMetaDataTester::Test()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // The write of the sort's copy for a message that is gone: an id a million
   // above the newest message the database has - the suite makes thousands of
   // messages in a minute, not a million in the milliseconds this takes - which
   // no row of hm_messages has, as a message deleted between the indexer's
   // query and its write has none. The write must succeed, as it inserts
   // nothing, and leave no row for the id. The fields carry what headers carry
   // - a quote, a backslash, a character outside ASCII - and a date, so that
   // the literals the write makes of them are read by the database at hand:
   // SQL Server Compact or SQL Server on the Windows bench. An error the write
   // reported would be in the error log, which the regression test that runs
   // the suite (MainOperations.TestInternals) holds empty.
   //---------------------------------------------------------------------------()
   {
      PersistentMessage persistentMessage;
      const __int64 gone = (__int64) persistentMessage.GetLatestMessageId() + 1000000;

      if (MetaDataRowsFor(gone) != 0)
         MetaDataFailed("a message id a million above the newest message already has a row of the sort's copy.");

      std::shared_ptr<MessageMetaData> metaData = std::make_shared<MessageMetaData>();
      metaData->SetAccountID(1);
      metaData->SetFolderID(1);
      metaData->SetMessageID(gone);
      metaData->SetDate(DateTime(2026, 10, 17, 6, 9, 53));
      metaData->SetFrom(_T("\"O'Brien, Pat\" <pat@example.test>"));
      metaData->SetSubject(_T("It's gone; a backslash \\ and a tilde ~ and r\u00e9sum\u00e9"));
      metaData->SetTo(_T("someone@example.test"));
      metaData->SetCC(_T(""));

      PersistentMessageMetaData persistentMetaData;

      if (!persistentMetaData.SaveObject(metaData))
         MetaDataFailed("the write of the sort's copy for a message that is gone failed, where it inserts nothing and succeeds.");

      if (MetaDataRowsFor(gone) != 0)
         MetaDataFailed("the write of the sort's copy for a message that is gone inserted a row.");
   }

}