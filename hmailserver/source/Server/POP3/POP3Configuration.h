// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class POP3Configuration
   {
   public:

      POP3Configuration();
      virtual ~POP3Configuration();

      long GetMaxPOP3Connections() const;
      void SetMaxPOP3Connections(int newVal);

      String GetWelcomeMessage() const;
      void SetWelcomeMessage(const String &sMessage);

      // The PROXY protocol on the POP3 listeners (POP3ProxyProtocolEnabled and
      // POP3ProxyProtocolTrustedIPs): when it is on and the connection's real peer
      // is on the list, the connection must open with a PROXY v1 or v2 header,
      // whose client address the session then has (TCPServer::HandleAccept).
      bool GetPOP3ProxyProtocolEnabled() const;
      void SetPOP3ProxyProtocolEnabled(bool newVal);

      String GetPOP3ProxyProtocolTrustedIPs() const;
      void SetPOP3ProxyProtocolTrustedIPs(const String &newVal);

   private:
      std::shared_ptr<PropertySet> GetSettings_() const;

   };

}
