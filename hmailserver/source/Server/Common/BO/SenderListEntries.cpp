// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SenderListEntries.h"
#include "../SQL/SQLCommand.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SenderListEntries::SenderListEntries() :
      scope_(SenderListEntry::ScopeServer),
      owner_id_(0)
   {
   }

   SenderListEntries::SenderListEntries(int scope, __int64 ownerId) :
      scope_(scope),
      owner_id_(ownerId)
   {
   }

   SenderListEntries::~SenderListEntries()
   {
   }

   std::shared_ptr<SenderListEntry>
   SenderListEntries::CreateEntry() const
   {
      std::shared_ptr<SenderListEntry> entry = std::shared_ptr<SenderListEntry>(new SenderListEntry());
      entry->SetScope(scope_);
      entry->SetOwnerID(owner_id_);

      return entry;
   }

   bool
   SenderListEntries::Refresh()
   {
      SQLCommand command("select * from hm_senderlist where slscope = @SCOPE and slownerid = @OWNERID order by slvalue asc");
      command.AddParameter("@SCOPE", scope_);
      command.AddParameter("@OWNERID", owner_id_);

      return DBLoad_(command);
   }

   bool
   SenderListEntries::PreSaveObject(std::shared_ptr<SenderListEntry> entry, XNode *node)
   {
      entry->SetScope(scope_);
      entry->SetOwnerID(owner_id_);

      return true;
   }

   bool
   SenderListEntries::RefreshAll()
   {
      // The whole table. It is a list people keep by hand - an address at a
      // time, with a reason typed next to it - so it is small by construction,
      // and one read that the cache keeps costs less than a scoped read per
      // recipient of every message.
      return DBLoad_(String("select * from hm_senderlist order by slscope asc, slownerid asc, slvalue asc"));
   }
}
