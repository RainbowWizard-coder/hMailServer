// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SenderListEntry.h"
#include "../AntiSpam/SenderPattern.h"
#include "../Util/Time.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SenderListEntry::SenderListEntry() :
      scope_(ScopeServer),
      owner_id_(0),
      // Allow is the default because the accident of an unset field must be
      // the harmless one: a row created without a kind and saved lets one
      // sender through, it does not silently start filing somebody's mail as
      // spam.
      kind_(KindAllow),
      expires_(0)
   {
      created_ = DateTime::GetCurrentTime();
   }

   SenderListEntry::~SenderListEntry()
   {
   }

   void
   SenderListEntry::SetValue(const String &newVal)
   {
      value_ = SenderPattern::Canonical(newVal);
   }

   bool
   SenderListEntry::Matches(const String &address) const
   {
      return SenderPattern::Matches(value_, address);
   }

   int
   SenderListEntry::Specificity() const
   {
      return SenderPattern::Specificity(value_);
   }

   bool
   SenderListEntry::HasExpired() const
   {
      if (!expires_)
         return false;

      // An expiry that is not a date is treated as expired rather than as
      // "never": the row said it should stop mattering at some point, and the
      // honest reading of an unreadable point is that it has passed. The
      // opposite reading would turn one bad write into a permanent exception.
      if (expires_time_.GetStatus() != DateTime::valid)
         return true;

      return expires_time_ < DateTime::GetCurrentTime();
   }

   bool
   SenderListEntry::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("SenderListEntry"));

      pNode->AppendAttr(_T("Scope"), StringParser::IntToString(scope_));
      pNode->AppendAttr(_T("OwnerID"), StringParser::IntToString(owner_id_));
      pNode->AppendAttr(_T("Kind"), StringParser::IntToString(kind_));
      pNode->AppendAttr(_T("Value"), value_);
      pNode->AppendAttr(_T("Reason"), reason_);
      pNode->AppendAttr(_T("Expires"), expires_ ? _T("1") : _T("0"));
      pNode->AppendAttr(_T("ExpiresTime"), Time::GetTimeStampFromDateTime(expires_time_));
      pNode->AppendAttr(_T("Created"), Time::GetTimeStampFromDateTime(created_));

      return true;
   }

   bool
   SenderListEntry::XMLLoad(XNode *pNode, int iOptions)
   {
      scope_ = _ttoi(pNode->GetAttrValue(_T("Scope")));
      owner_id_ = _ttoi64(pNode->GetAttrValue(_T("OwnerID")));
      kind_ = _ttoi(pNode->GetAttrValue(_T("Kind")));
      SetValue(pNode->GetAttrValue(_T("Value")));
      reason_ = pNode->GetAttrValue(_T("Reason"));
      expires_ = pNode->GetAttrValue(_T("Expires")) == _T("1") ? 1 : 0;
      expires_time_ = Time::GetDateFromSystemDate(pNode->GetAttrValue(_T("ExpiresTime")));
      created_ = Time::GetDateFromSystemDate(pNode->GetAttrValue(_T("Created")));

      return true;
   }
}
