// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "NeverBanNames.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   NeverBanNames::NeverBanNames()
   {
   }

   NeverBanNames::~NeverBanNames()
   {
   }

   bool
   NeverBanNames::Refresh()
   {
      // The whole table: provider names an administrator types in one at a
      // time, so small by construction.
      return DBLoad_(String("select * from hm_neverbannames order by nbnpattern asc"));
   }
}
