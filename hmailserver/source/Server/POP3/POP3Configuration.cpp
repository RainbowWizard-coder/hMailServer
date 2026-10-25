// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "POP3Configuration.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   POP3Configuration::POP3Configuration()
   {

   }

   POP3Configuration::~POP3Configuration()
   {

   }

   std::shared_ptr<PropertySet> 
   POP3Configuration::GetSettings_() const
   {
      return Configuration::Instance()->GetSettings();
   }

   void
   POP3Configuration::SetMaxPOP3Connections(int newVal)
   {
      GetSettings_()->SetLong(PROPERTY_MAXPOP3CONNECTIONS, newVal);
   }

   long
   POP3Configuration::GetMaxPOP3Connections() const
   {
      return GetSettings_()->GetLong(PROPERTY_MAXPOP3CONNECTIONS);
   }


   String 
   POP3Configuration::GetWelcomeMessage() const
   {
      return GetSettings_()->GetString(PROPERTY_WELCOMEPOP3);
   }

   void 
   POP3Configuration::SetWelcomeMessage(const String &sMessage)
   {
      GetSettings_()->SetString(PROPERTY_WELCOMEPOP3, sMessage);
   }

   bool
   POP3Configuration::GetPOP3ProxyProtocolEnabled() const
   {
      return GetSettings_()->GetBool(PROPERTY_POP3_PROXY_PROTOCOL_ENABLED);
   }

   void
   POP3Configuration::SetPOP3ProxyProtocolEnabled(bool newVal)
   {
      GetSettings_()->SetBool(PROPERTY_POP3_PROXY_PROTOCOL_ENABLED, newVal);
   }

   String
   POP3Configuration::GetPOP3ProxyProtocolTrustedIPs() const
   {
      return GetSettings_()->GetString(PROPERTY_POP3_PROXY_PROTOCOL_TRUSTED_IPS);
   }

   void
   POP3Configuration::SetPOP3ProxyProtocolTrustedIPs(const String &newVal)
   {
      GetSettings_()->SetString(PROPERTY_POP3_PROXY_PROTOCOL_TRUSTED_IPS, newVal);
   }
}