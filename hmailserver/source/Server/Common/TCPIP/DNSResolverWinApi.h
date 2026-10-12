// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DNSRecord.h"

namespace HM
{


   class DNSResolverWinApi
   {
   public:
      DNSResolverWinApi();
      virtual ~DNSResolverWinApi();

      bool Query(const String &query, int resourceType, std::vector<DNSRecord> &foundRecords);

      // bypassCache asks the Windows DNS client to go to the DNS server rather
      // than answer from its cache, so that a lookup asked again after a failure
      // is asked again (DNSResolver::GetIpAddressesWithRetry). UseDNSCache=0 and
      // a DNSServer bypass the cache on every query already. The POSIX resolver
      // keeps no cache, and ignores it.
      bool Query(const String &query, int resourceType, std::vector<DNSRecord> &foundRecords, int &outStatus, bool bypassCache = false);

   private:

      bool RunQuery_(const String &query, int resourceType, unsigned long fOptions, std::vector<DNSRecord> &foundRecords, int &dnsStatus);

      bool IsDNSError_(int iErrorMessage);
   };


}
