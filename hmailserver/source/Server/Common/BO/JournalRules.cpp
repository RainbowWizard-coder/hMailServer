// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "JournalRules.h"
#include "../SQL/SQLCommand.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   JournalRules::JournalRules() :
      domain_id_(0)
   {
   }

   JournalRules::JournalRules(__int64 domainId) :
      domain_id_(domainId)
   {
   }

   JournalRules::~JournalRules()
   {
   }

   bool
   JournalRules::Refresh()
   {
      if (domain_id_ == 0)
      {
         // The whole table: rules an administrator writes one at a time, so small
         // by construction, and one read Journaling keeps costs less than a
         // query for every accepted message.
         return DBLoad_(String("select * from hm_journalrules order by jrdomainid asc, jrid asc"));
      }

      SQLCommand command("select * from hm_journalrules where jrdomainid = @DOMAINID order by jrid asc");
      command.AddParameter("@DOMAINID", domain_id_);

      return DBLoad_(command);
   }

   bool
   JournalRules::PreSaveObject(std::shared_ptr<JournalRule> rule, XNode *node)
   {
      // The restore's domain id, whatever the archive recorded. The account of an
      // account rule is found again by its address in PersistentJournalRule's
      // restore save, which is told the domain to look under by this.
      rule->SetDomainID(domain_id_);
      rule->SetAccountID(0);
      return true;
   }
}
