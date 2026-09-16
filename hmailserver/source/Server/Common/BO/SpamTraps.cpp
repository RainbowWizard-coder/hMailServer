// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamTraps.h"
#include "../SQL/SQLCommand.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SpamTraps::SpamTraps() :
      domain_id_(0)
   {
   }

   SpamTraps::SpamTraps(__int64 domainId) :
      domain_id_(domainId)
   {
   }

   SpamTraps::~SpamTraps()
   {
   }

   bool
   SpamTraps::Refresh()
   {
      SQLCommand command("select * from hm_spamtraps where stdomainid = @DOMAINID order by stlocalpart asc");
      command.AddParameter("@DOMAINID", domain_id_);

      return DBLoad_(command);
   }

   bool
   SpamTraps::RefreshAll()
   {
      // The whole table: addresses an administrator types in one at a time, so
      // small by construction, and one read the cache keeps costs less than a
      // query for every RCPT TO.
      return DBLoad_(String("select * from hm_spamtraps order by stdomainid asc, stlocalpart asc"));
   }

   bool
   SpamTraps::PreSaveObject(std::shared_ptr<SpamTrap> trap, XNode *node)
   {
      trap->SetDomainID(domain_id_);
      return true;
   }
}
