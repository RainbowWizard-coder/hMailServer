// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class FetchAccount;
   class FetchAccountUIDs;

   class PersistentFetchAccount
   {
   public:
      PersistentFetchAccount(void);
      ~PersistentFetchAccount(void);

      static void Lock(__int64 ID);
      static void Unlock(__int64 ID);
      static void UnlockAll();
      static bool IsLocked(__int64 ID);
      static bool ReadObject(std::shared_ptr<FetchAccount> pFA, const SQLCommand& command);
      static bool ReadObject(std::shared_ptr<FetchAccount> oFA, std::shared_ptr<DALRecordset> pRS);
      static bool SaveObject(std::shared_ptr<FetchAccount> oFA, String &errorMessage,PersistenceMode mode);
      static bool SaveObject(std::shared_ptr<FetchAccount> oFA);
      static bool DeleteObject(std::shared_ptr<FetchAccount> pFA);
      static void DeleteByAccountID(__int64 ID);

      static void SetRetryNow(__int64 iFAID);

      // One column each, written without the rest of the row: the fetcher's,
      // which must not put back a change the reader saved while it ran.
      static void SaveOAuthToken(__int64 iFAID, const String &token);
      static void SaveMirrorSeen(__int64 iFAID, const String &seen);

      // How many messages the fetch account has on record as collected.
      static int CountUIDs(__int64 iFAID);
      static void SetNextTryTime(std::shared_ptr<FetchAccount> pFA);
   };
}