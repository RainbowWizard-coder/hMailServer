// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "WhiteListAddress.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   WhiteListAddress::WhiteListAddress(void)
   {
      
   }

   WhiteListAddress::~WhiteListAddress(void)
   {
   }
   
   String
   WhiteListAddress::GetName() const
   {  
      String sName = white_lower_ipaddress_.ToString() + white_upper_ipaddress_.ToString();
      return sName;
   }

   bool 
   WhiteListAddress::XMLStore(XNode *pParentNode, int iOptions)
   {
      XNode *pNode = pParentNode->AppendChild(_T("WhiteAddress"));

      pNode->AppendAttr(_T("Name"), GetName());
      pNode->AppendAttr(_T("LowerIPAddress"), String(white_lower_ipaddress_.ToString()));
      pNode->AppendAttr(_T("UpperIPAddress"), String(white_upper_ipaddress_.ToString()));
      pNode->AppendAttr(_T("EMailAddress"), white_email_address_);
      pNode->AppendAttr(_T("Description"), white_description_);

      return true;

      
   }

   bool 
   WhiteListAddress::XMLLoad(XNode *pNode, int iOptions)
   {
      white_lower_ipaddress_.TryParse(pNode->GetAttrValue(_T("LowerIPAddress")));
      white_upper_ipaddress_.TryParse(pNode->GetAttrValue(_T("UpperIPAddress")));
      white_email_address_ = pNode->GetAttrValue(_T("EMailAddress"));
      white_description_ = pNode->GetAttrValue(_T("Description"));

      return true;
   }

   // As SecurityRange's: a text that is no address leaves the bound as it was,
   // never 0.0.0.0, which is where TryParse leaves an address it refuses - and a
   // lower bound of 0.0.0.0 white-lists every address below the upper one.
   bool
   WhiteListAddress::SetLowerIPAddress(const String &iIPAddress)
   {
      IPAddress parsed;
      if (!parsed.TryParse(iIPAddress))
         return false;

      white_lower_ipaddress_ = parsed;
      return true;
   }

   bool
   WhiteListAddress::SetUpperIPAddress(const String &iIPAddress)
   {
      IPAddress parsed;
      if (!parsed.TryParse(iIPAddress))
         return false;

      white_upper_ipaddress_ = parsed;
      return true;
   }
}
