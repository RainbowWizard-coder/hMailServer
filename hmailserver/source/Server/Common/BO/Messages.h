// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collection.h"

#include "../BO/Message.h"
#include "../Persistence/PersistentMessage.h"

#include <functional>
#include <set>
#include <vector>

namespace HM
{

   class Messages : public Collection<Message, PersistentMessage>
   {
   public:
      Messages(__int64 iAccountID, __int64 iFolderID);
      virtual ~Messages();

      void Save();

      long GetSize() const;
      __int64 GetFirstUnseenUID() const;

      // 1-based sequence number of the first message without the \Seen flag, or
      // 0 when there is none. RFC 3501 requires a sequence number (not a UID) in
      // the [UNSEEN] response code of SELECT/EXAMINE.
      __int64 GetFirstUnseenSequenceNumber() const;

      // The ids of the messages currently carrying \Recent, collected under the
      // collection lock. Callers must not walk GetVector() themselves: another
      // session can erase from the shared vector while it is being iterated.
      std::set<__int64> GetRecentMessageIDs() const;

      long GetNoOfSeen() const;

      std::vector<std::shared_ptr<Message>> GetCopy();

      // Walks the collection from its last message to its first - newest UID
      // first, the order a listing is read in - under the collection's lock,
      // handing visit each message not flagged \Deleted until visit answers
      // false. Returns how many messages are not flagged \Deleted, counted over
      // the whole collection whether visit stopped early or not, so one pass
      // gives a listing both its page and its total.
      //
      // This is what lets a folder of a hundred thousand be listed a page at a
      // time: GetCopy made a copy of every message for every page, under this
      // same lock, and a visitor copies only what it keeps (new Message(*m)).
      // What visit does runs under the lock every IMAP session on the folder
      // waits on, so it must stay cheap: compare, collect an id, copy a
      // message. It must never read a file, and never call back into this
      // collection.
      size_t VisitNewestFirst(const std::function<bool(const std::shared_ptr<Message> &)> &visit) const;

      // Copies of the messages with the given database ids, keyed on id, in ONE pass over
      // the collection: a command resolving a whole message set must not scan it once per
      // id. GetItemsByIds hands out the objects the collection holds instead, for the
      // commands that update them (STORE, and FETCH setting \Seen).
      std::map<__int64, std::shared_ptr<Message>> GetCopyByIds(const std::set<__int64> &message_ids) const;
      std::map<__int64, std::shared_ptr<Message>> GetItemsByIds(const std::set<__int64> &message_ids) const;
      std::shared_ptr<Message> GetCopyByDBID(__int64 message_id) const;

      std::shared_ptr<Message> GetItemByUID(unsigned int uid);
      std::shared_ptr<Message> GetItemByUID(unsigned int uid, unsigned int &foundIndex);

      // Deletes the messages the filter selects and returns the database ids of those
      // actually deleted - which is what a change notification carries, since a
      // sequence number means a different message to every session.
      std::vector<__int64> DeleteMessages(const std::function<bool(std::shared_ptr<Message>)> &filter);
      std::vector<__int64> DeleteMessagesById(const std::set<__int64> &message_ids);

      // False when the messages could not be loaded, so the caller can retry
      // instead of treating an empty collection as an empty folder.
      bool Refresh(bool update_recent_flags);

      void AddToCollection(std::shared_ptr<DALRecordset> pRS);

      void Remove(__int64 iDBID);

      void RemoveRecentFlags();

      // RFC 3501 2.3.2: \Recent belongs to "the first session to have been
      // notified about this message". The ids of the messages carrying it, taken
      // off them under the collection lock - in memory and in the database, as
      // RemoveRecentFlags does, though in the database only up to the highest id
      // claimed - so that exactly one session claims each.
      std::set<__int64> ClaimRecentMessageIDs();

      __int64 GetAccountID() {return account_id_; }
      __int64 GetFolderID() {return folder_id_; }

   protected:
      virtual String GetCollectionName() const {return "Messages"; }
      virtual bool PreSaveObject(std::shared_ptr<Message> pMessage, XNode *node);
   private:

      unsigned int last_refreshed_uid_;

      __int64 account_id_;
      __int64 folder_id_;
   };
}
