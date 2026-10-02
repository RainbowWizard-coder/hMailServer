// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../TCPIP/IPAddress.h"
#include "PersistenceMode.h"

namespace HM
{
   class SecurityRange;

   class PersistentSecurityRange
   {
   public:
      PersistentSecurityRange();
      virtual ~PersistentSecurityRange();

      static bool DeleteObject(std::shared_ptr<SecurityRange> pSR);
      static bool SaveObject(std::shared_ptr<SecurityRange> pSR);
      static bool SaveObject(std::shared_ptr<SecurityRange> pSR, String &result,  PersistenceMode mode);

      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, std::shared_ptr<DALRecordset> pRS);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, const SQLCommand &command);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, __int64 ObjectID);
      static bool ReadObject(std::shared_ptr<SecurityRange> pSR, const String &name);

      static std::shared_ptr<SecurityRange> ReadMatchingIP(const IPAddress &ipaddress);
      
      static bool DeleteExpired();

      static bool Exists(const String &name);

      // An auto-ban's row widened to lower..upper (Util/AutoBanWidener.h): the
      // row with that id, and only while it is still the single expiring IPv4
      // address it was written as - a row an administrator has edited since, or
      // one already lifted, is not touched. All three addresses IPv4. True when
      // the statement ran; the caller reads the row back to learn whether it
      // matched.
      static bool WidenAutoBan(__int64 id, const IPAddress &address, const IPAddress &lower, const IPAddress &upper);

   private:

      static bool Validate(std::shared_ptr<SecurityRange> pSR, String &result);
   };

}
