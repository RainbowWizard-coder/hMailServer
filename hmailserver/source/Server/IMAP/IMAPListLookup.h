// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class IMAPListLookup
   {
   public:

      IMAPListLookup();
      virtual ~IMAPListLookup();

      // maxItem is the value "*" stands for: the largest UID in the mailbox for a
      // UID set, or the number of messages for a message-sequence set. 64-bit,
      // because a UID is an unsigned 32-bit number: "UID 1:4294967295" was read
      // through an int, overflowed, and matched only the first message.
      static bool IsItemInList(const std::vector<String> &vecItems, __int64 item, __int64 maxItem);

   private:

   };


}