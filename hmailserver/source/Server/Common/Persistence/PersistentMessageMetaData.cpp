// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "./PersistentMessageMetaData.h"
#include "./PersistentMessage.h"
#include "../BO/Message.h"

#include "../SQL/SQLStatement.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/DatabaseTransaction.h"
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
   // PersistentMessageIndex::InsertTerm_ writes a term of the full-text index.
   // A message that is deleted between the indexer's query and this write - a
   // message expunged or a mailbox emptied while the indexer catches up, the
   // regression suite's setup clearing the last test's mail at the start of the
   // next while the indexer indexes it - then inserts nothing and succeeds,
   // which is the truth of it: the row of a message that is gone is not wanted,
   // and the indexer's next pass does not ask for it. The plain INSERT of the
   // values this was, in every version to 6.3.6, was refused for such a message
   // by the foreign key hm_message_metadata has had on hm_messages since schema
   // 6030 - "Value violated the integrity constraints" on SQL Server Compact -
   // which DALConnection::Execute reported at High as HM5032, and the indexer
   // gave the batch up (MessageIndexer::IndexMessages_). Seen twice in batch48's
   // windows-gate-s1vm run of 17 October 2026, both at a test's setup.
   //
   // The statement carries no parameter: the message id is written as a number,
   // and the date and the four header fields as literals, since SQL Server
   // Compact allows no parameter in a select list. A statement with a parameter
   // is read whole for the parameter's name - by SQLStatement::GenerateFromCommand
   // on MySQL and PostgreSQL, which put the values into the text, and by
   // ADOConnection on SQL Server, which puts a ? for the name - and a header that
   // held the name would have been rewritten (the re-check of 17 October 2026).
   // The header fields are cut to a hundred characters, as the INSERT cut them,
   // their quotes doubled by SQLStatement::Escape, which doubles backslashes too
   // where the engine reads them as escapes (MySQL, and PostgreSQL without
   // standard_conforming_strings), and they are N'...' on SQL Server and Compact,
   // where a bare '...' is the database's code page. There a backslash that a
   // line break follows inside a literal is a line continuation, and both are
   // dropped - a lone CR or LF reaches a decoded header from an encoded word - so
   // on those two every backslash is written as NCHAR(92) between two literals.
   // The two readers of a statement's literals, DatabaseConnectionManager::
   // RedactSqlLiterals_ and DALConnection::HasIgnoreErrorsMarker, read each
   // literal as the engine does (DALConnection::EndOfLiteral). A date older
   // than 1800, or none, is NULL, as SQLStatement::AddColumnDate makes it. The
   // account and folder are read from the message's row, the ones the indexer's
   // query read. Nothing reads the row's id after the write, which the INSERT
   // fetched. A failure of the write - a timeout, a lost connection - is reported
   // with the statement's text, the fields in it, on SQL Server and Compact as it
   // always was on MySQL and PostgreSQL. MessageMetaDataTester holds the fields
   // to a verbatim round trip and a message that is gone to no row.
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

         String escaped = SQLStatement::Escape(cut);

         if (unicodeLiteral)
            escaped.Replace(_T("\\"), _T("' + NCHAR(92) + N'"));

         return String(unicodeLiteral ? _T("N'") : _T("'")) + escaped + _T("'");
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
      sql += _T(" from hm_messages where messageid = ") + StringParser::IntToString(metaData->GetMessageID());

      return Application::Instance()->GetDBManager()->Execute(SQLCommand(sql));
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

   namespace
   {
      // Takes the tester's rows out again on every way out, for a database
      // that is not transactional (MySQL's MyISAM), where the transaction's
      // roll-back undoes nothing; on the others the roll-back, which comes
      // after this, undoes the deletes with the rest.
      struct TesterRows
      {
         std::shared_ptr<DatabaseConnectionManager> manager;
         __int64 messageId = 0;

         ~TesterRows()
         {
            if (messageId <= 0)
               return;

            try
            {
               SQLCommand metaData(_T("delete from hm_message_metadata where metadata_messageid = @MESSAGEID"));
               metaData.AddParameter("@MESSAGEID", messageId);
               manager->Execute(metaData);

               SQLCommand message(_T("delete from hm_messages where messageid = @MESSAGEID"));
               message.AddParameter("@MESSAGEID", messageId);
               manager->Execute(message);
            }
            catch (...)
            {
               // A destructor, on the way out of the test: the transaction's
               // roll-back, which follows, takes the rows on every engine but
               // a MySQL database that is not transactional.
               LOG_DEBUG("MessageMetaDataTester: the test's rows could not be taken out by hand; the transaction's roll-back takes them.");
            }
         }
      };
   }

   void
   MessageMetaDataTester::Test()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Two writes of the sort's copy, against the database at hand - SQL Server
   // Compact or SQL Server on the Windows bench, where ClassTester runs. An
   // error either write reported would be in the error log, which the
   // regression test that runs the suite (MainOperations.TestInternals) holds
   // empty.
   //
   // 1. A message that is gone: an id a million above the newest message the
   //    database has - the suite makes thousands of messages in a minute, not a
   //    million in the milliseconds this takes - which no row of hm_messages
   //    has, as a message deleted between the indexer's query and its write has
   //    none. The write must succeed, as it inserts nothing, and leave no row
   //    for the id.
   //
   // 2. A message that exists: a row of hm_messages written for the test, in a
   //    transaction rolled back at the end (the rows taken out by hand too, for
   //    a database that is not transactional), of a type the indexer's query
   //    does not ask for. The write must succeed and leave one row, with the
   //    date and the four fields read back verbatim: the fields carry the
   //    name of a parameter, a quote, backslashes - one before a line break,
   //    one at the end - a comment's delimiters and the DAL's own marker text,
   //    a tilde and a character outside ASCII, what headers carry and what the
   //    re-check of 17 October 2026 found the first literals lose or rewrite
   //    (SaveObject says how). The account and folder read back are the
   //    message's.
   //---------------------------------------------------------------------------()
   {
      PersistentMessageMetaData persistentMetaData;

      // 1. A message that is gone.
      PersistentMessage persistentMessage;
      const __int64 gone = (__int64) persistentMessage.GetLatestMessageId() + 1000000;

      if (MetaDataRowsFor(gone) != 0)
         MetaDataFailed("a message id a million above the newest message already has a row of the sort's copy.");

      std::shared_ptr<MessageMetaData> ofGone = std::make_shared<MessageMetaData>();
      ofGone->SetAccountID(1);
      ofGone->SetFolderID(1);
      ofGone->SetMessageID(gone);
      ofGone->SetDate(DateTime(2026, 10, 17, 6, 9, 53));
      ofGone->SetFrom(_T("\"O'Brien, Pat\" <pat@example.test>"));
      ofGone->SetSubject(_T("It's gone; a backslash \\ and a tilde ~ and r\u00e9sum\u00e9"));
      ofGone->SetTo(_T("someone@example.test"));
      ofGone->SetCC(_T(""));

      if (!persistentMetaData.SaveObject(ofGone))
         MetaDataFailed("the write of the sort's copy for a message that is gone failed, where it inserts nothing and succeeds.");

      if (MetaDataRowsFor(gone) != 0)
         MetaDataFailed("the write of the sort's copy for a message that is gone inserted a row.");

      // 2. A message that exists.
      std::shared_ptr<DatabaseConnectionManager> manager = Application::Instance()->GetDBManager();
      if (!manager)
         MetaDataFailed("there is no database manager to write the sort's copy with.");

      DatabaseTransaction transaction(manager);
      if (!transaction.IsOpen())
         MetaDataFailed("a transaction for the sort's copy of a message that exists could not be begun.");

      TesterRows rows = { manager, 0 };

      SQLStatement row;
      row.SetTable("hm_messages");
      row.SetStatementType(SQLStatement::STInsert);
      row.SetIdentityColumn("messageid");
      row.AddColumnInt64("messageaccountid", 0);
      row.AddColumnInt64("messagefolderid", 0);
      row.AddColumn("messagefilename", String(_T("MessageMetaDataTester")));
      row.AddColumn("messagetype", 1);
      row.AddColumn("messagefrom", String(_T("tester@example.test")));
      row.AddColumn("messagesize", 0);
      row.AddColumn("messageflags", 0);
      row.AddColumn("messagekeywords", String());
      row.AddColumnInt64("messagesubmitterid", 0);
      row.AddColumn("messageauthdomains", String());
      row.AddColumnInt64("messageuid", 0);
      row.AddColumnInt64("messagemodseq", 0);
      row.AddColumn("messagelocked", 0);
      row.AddColumn("messagecreatetime", Time::GetCurrentDateTime());
      row.AddColumn("messagesavedate", Time::GetCurrentDateTime());
      row.AddColumn("messageemailid", String(_T("MMessageMetaDataTester")));
      row.AddColumn("messagecurnooftries", 0);
      row.AddColumn("messagenexttrytime", String(_T("1901-01-01")));

      __int64 messageId = 0;
      if (!transaction.Execute(row, &messageId) || messageId <= 0)
         MetaDataFailed("a message row for the sort's copy of a message that exists could not be written.");

      rows.messageId = messageId;

      const String from = _T("\"O'Brien, Pat\" <pat@example.test> \\");
      const String subject = _T("Ticket @MESSAGEID resolved: C:\\Reports\\\nnext line; a tilde ~ and r\u00e9sum\u00e9");
      const String to = _T("root@MESSAGEIDS.example");
      const String cc = _T("/* [IGNORE-ERRORS] */ cc@example.test -- ' \" `");

      std::shared_ptr<MessageMetaData> ofExisting = std::make_shared<MessageMetaData>();
      ofExisting->SetAccountID(0);
      ofExisting->SetFolderID(0);
      ofExisting->SetMessageID(messageId);
      ofExisting->SetDate(DateTime(2026, 10, 17, 6, 9, 53));
      ofExisting->SetFrom(from);
      ofExisting->SetSubject(subject);
      ofExisting->SetTo(to);
      ofExisting->SetCC(cc);

      if (!persistentMetaData.SaveObject(ofExisting))
         MetaDataFailed("the write of the sort's copy for a message that exists failed.");

      if (MetaDataRowsFor(messageId) != 1)
         MetaDataFailed("the write of the sort's copy for a message that exists did not leave one row.");

      SQLCommand read(_T("select metadata_accountid, metadata_folderid, metadata_from, metadata_subject, metadata_to, metadata_cc from hm_message_metadata where metadata_messageid = @MESSAGEID and metadata_dateutc = '2026-10-17 06:09:53'"));
      read.AddParameter("@MESSAGEID", messageId);

      std::shared_ptr<DALRecordset> pRS = manager->OpenRecordset(read);
      if (!pRS || pRS->IsEOF())
         MetaDataFailed("the sort's copy for a message that exists was not read back by its id and its date.");

      if (pRS->GetLongValue("metadata_accountid") != 0 || pRS->GetLongValue("metadata_folderid") != 0)
         MetaDataFailed("the sort's copy for a message that exists does not carry the message's account and folder.");

      if (pRS->GetStringValue("metadata_from") != from)
         MetaDataFailed("the From of the sort's copy did not come back as written: a value that ends in a backslash.");

      if (pRS->GetStringValue("metadata_subject") != subject)
         MetaDataFailed("the Subject of the sort's copy did not come back as written: a parameter's name, a backslash before a line break, a character outside ASCII.");

      if (pRS->GetStringValue("metadata_to") != to)
         MetaDataFailed("the To of the sort's copy did not come back as written: a parameter's name inside an address.");

      if (pRS->GetStringValue("metadata_cc") != cc)
         MetaDataFailed("the Cc of the sort's copy did not come back as written: a comment's delimiters, the marker text, quotes.");

      pRS.reset();

      // The rows go with TesterRows, then the transaction rolls back.
   }

}