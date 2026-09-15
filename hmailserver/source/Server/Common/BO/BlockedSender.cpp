// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "BlockedSender.h"
#include "../AntiSpam/SenderPattern.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   BlockedSender::BlockedSender(void) :
      // The default score is far above the shipped SpamDeleteThreshold (20),
      // so that an entry created without an explicit score does what the
      // administrator who created it meant: it blocks. An administrator who
      // wants a listed sender merely scored rather than refused lowers the
      // score on the entry.
      score_(100)
   {
   }

   BlockedSender::~BlockedSender(void)
   {
   }

   bool
   BlockedSender::Matches(const String &sFromAddress) const
   {
      // The rule lives in SenderPattern now, which is where the reader's own
      // allow and block lists and the domain's and the server's read it too.
      // It was written here first and moved rather than copied: two answers to
      // "does example.com cover mail.example.com" would be two lists an
      // administrator had to learn separately, and the day one of them was
      // fixed the other would quietly keep the bug. Nothing about what matches
      // changed in the move - the null sender still matches nothing, an entry
      // with an @ is still one exact address, and a domain still covers its
      // subdomains at a label boundary.
      return SenderPattern::Matches(address_, sFromAddress);
   }

   bool
   BlockedSender::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("BlockedSender"));

      pNode->AppendAttr(_T("Name"), GetName());
      pNode->AppendAttr(_T("Address"), address_);
      pNode->AppendAttr(_T("Score"), StringParser::IntToString(score_));
      pNode->AppendAttr(_T("Description"), description_);

      return true;
   }

   bool
   BlockedSender::XMLLoad(XNode *pNode, int iOptions)
   {
      address_ = pNode->GetAttrValue(_T("Address"));
      score_ = _ttoi(pNode->GetAttrValue(_T("Score")));
      description_ = pNode->GetAttrValue(_T("Description"));

      return true;
   }
}
