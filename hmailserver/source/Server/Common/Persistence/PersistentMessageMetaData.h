// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../BO/MessageMetaData.h"

namespace HM
{
   class Message;

   class PersistentMessageMetaData
   {
   public:

      class MessageInfo
      {
      public:
         __int64 MessageID = 0;
         int AccountID = 0;
         int FolderID = 0;
         String FileName;
      };

      PersistentMessageMetaData(void);
      ~PersistentMessageMetaData(void);

      // quickIndex param added to choose new faster/limiting option
      std::set<std::shared_ptr<MessageInfo> > GetMessagesToIndex(bool quickIndex);

      bool DeleteForMessage(std::shared_ptr<Message> message);
      void GetMetaData(int accountID, int folderID, const String &headerField, std::map<__int64, String > &result);

      // The sort's copy of a message's headers, written as an INSERT ... SELECT
      // from the message's own row: a message deleted since the indexer's query
      // inserts nothing and succeeds (the .cpp says why). False when the
      // database refused or failed the write, which is reported as HM5032.
      bool SaveObject(std::shared_ptr<MessageMetaData> metaData);

      // The metadata of messages that no longer exist, a thousand message ids a
      // statement with a pause between two, so that a stop of the servers waits
      // for a slice rather than the whole table (the .cpp says why). Run by the
      // message indexer's thread.
      bool DeleteOrphanedItems();
      int GetTotalMessageCount();
      void Clear();
   };

   // SaveObject for a message that is gone - an id above every message the
   // database has - inserts nothing and succeeds, and leaves no row for that id;
   // and for a message that exists - a row of the tester's own, in a transaction
   // rolled back - leaves one row, whose date and four fields read back verbatim:
   // a parameter's name, quotes, a backslash before a line break and one at the
   // end, a comment's delimiters and the DAL's marker text, a character outside
   // ASCII, as headers carry them. Runs inside the server on Windows, through
   // ClassTester, against the bench's database. Throws std::logic_error naming
   // what did not hold.
   class MessageMetaDataTester
   {
   public:
      void Test();
   };
}