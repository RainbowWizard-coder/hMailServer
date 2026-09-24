// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "DistributionListRecipient.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DistributionListRecipient::DistributionListRecipient(void) :
      list_id_(0),
      delivery_(DeliveryEach),
      subscribed_(0),
      confirmed_(Confirmed),
      hard_bounces_(0),
      soft_bounces_(0),
      soft_since_(0),
      last_bounce_(0)
   {

   }

   DistributionListRecipient::~DistributionListRecipient(void)
   {
   }

   void
   DistributionListRecipient::SetAddress(const String & sAddress)
   {
      address_ = sAddress;
   }

   bool 
   DistributionListRecipient::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("Recipient"));

      pNode->AppendAttr(_T("Name"), address_);

      // The list manager's view of the member (ListManager.h). The token is in
      // the backup so that the links in mail already sent still work after a
      // restore; the backup holds the passwords' hashes too, and is kept
      // accordingly.
      pNode->AppendAttr(_T("Delivery"), StringParser::IntToString(delivery_));
      pNode->AppendAttr(_T("Confirmed"), StringParser::IntToString(confirmed_));
      pNode->AppendAttr(_T("Subscribed"), StringParser::IntToString(subscribed_));
      pNode->AppendAttr(_T("Token"), token_);
      pNode->AppendAttr(_T("HardBounces"), StringParser::IntToString(hard_bounces_));
      pNode->AppendAttr(_T("SoftBounces"), StringParser::IntToString(soft_bounces_));
      pNode->AppendAttr(_T("SoftSince"), StringParser::IntToString(soft_since_));
      pNode->AppendAttr(_T("LastBounce"), StringParser::IntToString(last_bounce_));
      pNode->AppendAttr(_T("Reason"), reason_);

      return true;
   }

   bool 
   DistributionListRecipient::XMLLoad(XNode *pNode, int iOptions)
   {
      address_ = pNode->GetAttrValue(_T("Name"));

      // Absent in a backup taken before the list manager: a member an
      // administrator added, confirmed and sent every message - which is what
      // an empty Confirmed must read as, since 0 would be a request nobody
      // confirmed.
      delivery_ = _ttoi(pNode->GetAttrValue(_T("Delivery")));
      const String confirmed = pNode->GetAttrValue(_T("Confirmed"));
      confirmed_ = confirmed.IsEmpty() ? Confirmed : _ttoi(confirmed);
      subscribed_ = _ttoi64(pNode->GetAttrValue(_T("Subscribed")));
      token_ = pNode->GetAttrValue(_T("Token"));
      hard_bounces_ = _ttoi(pNode->GetAttrValue(_T("HardBounces")));
      soft_bounces_ = _ttoi(pNode->GetAttrValue(_T("SoftBounces")));
      soft_since_ = _ttoi64(pNode->GetAttrValue(_T("SoftSince")));
      last_bounce_ = _ttoi64(pNode->GetAttrValue(_T("LastBounce")));
      reason_ = pNode->GetAttrValue(_T("Reason"));

      return true;
   }
}