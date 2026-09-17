// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// A mailbox written to a directory as .eml files, one per message, in
// directories mirroring the folder tree: what a person exercising a
// data-access right is handed, and what Account.ExportMessages and
// POST /api/v1/accounts/{address}/export both do. See MessageExport.cpp.
#pragma once

namespace HM
{
   class Account;

   class MessageExport
   {
   public:
      // Copies every message of the account into directory, mirroring the
      // folder tree, and counts them. Stops on the FIRST failure with error
      // saying which file, because a partial export that reports success is
      // exactly the wrong artefact to hand over. The folder tree is read fresh.
      static bool ExportAccount(std::shared_ptr<Account> account, const String &directory, int &exported, String &error);
   };
}
