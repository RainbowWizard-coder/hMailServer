// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SpamTrap.h"
#include "../Util/Time.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SpamTrap::SpamTrap() :
      domain_id_(0)
   {
      created_ = DateTime::GetCurrentTime();
   }

   SpamTrap::~SpamTrap()
   {
   }

   void
   SpamTrap::SetLocalPart(const String &newVal)
   {
      String value = newVal;
      value.Trim();
      value.ToLower();
      local_part_ = value;
   }

   bool
   SpamTrap::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("SpamTrap"));

      pNode->AppendAttr(_T("LocalPart"), local_part_);
      pNode->AppendAttr(_T("Created"), Time::GetTimeStampFromDateTime(created_));

      return true;
   }

   bool
   SpamTrap::XMLLoad(XNode *pNode, int iOptions)
   {
      SetLocalPart(pNode->GetAttrValue(_T("LocalPart")));

      // A backup that carries no readable time restores the trap as declared
      // now, which is the one reading that cannot make it look older than it is.
      created_ = Time::GetDateFromSystemDate(pNode->GetAttrValue(_T("Created")));
      if (created_.GetStatus() != DateTime::valid)
         created_ = DateTime::GetCurrentTime();

      return true;
   }
}
