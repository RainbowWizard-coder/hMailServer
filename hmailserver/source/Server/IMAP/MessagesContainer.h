// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../Common/Cache/Cache.h"

#include <memory>
#include <set>

namespace HM
{
   class Messages;
   class CachedMessages;

   class MessagesContainer : public Singleton<MessagesContainer>
   {
   public:

      MessagesContainer();

      std::shared_ptr<Messages> GetMessages(__int64 account_id, __int64 folder_id);
      std::shared_ptr<Messages> GetMessages(__int64 account_id, __int64 folder_id, std::set<__int64> &recent_messages, bool update_recent_messages);

      void SetFolderNeedsRefresh(__int64 folder_id);
      void UncacheAccount(__int64 account_id);
      void Clear();

   private:

      // Both overloads above, the recent ids collected only when a set to put
      // them in is passed.
      std::shared_ptr<Messages> GetMessages_(__int64 account_id, __int64 folder_id, std::set<__int64> *recent_messages, bool update_recent_messages);

      Cache<CachedMessages> messages_cache_;


   };
}
