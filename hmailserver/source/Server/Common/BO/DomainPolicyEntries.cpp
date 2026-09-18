// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DomainPolicyEntries.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DomainPolicyEntries::DomainPolicyEntries()
   {
   }

   DomainPolicyEntries::~DomainPolicyEntries()
   {
   }

   bool
   DomainPolicyEntries::Refresh()
   {
      // The whole table: domains an administrator types in one at a time, so
      // small by construction - "a handful of rows is the whole operational
      // point", as the discussion that asked for it put it.
      return DBLoad_(String("select * from hm_domainpolicylist order by dplpattern asc, dplmatchon asc"));
   }
}
