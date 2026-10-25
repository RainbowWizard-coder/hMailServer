// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The rules an ACME check is decided by (AcmeChecksTester, Common/Util/AcmeClient.h)
// as a program of its own for the POSIX build - the counterpart of dane-tests.
//
// On Windows AcmeChecksTester runs inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches). That route does not exist
// here, so the CMake build makes this and linux-build.sh runs it with every other
// *-tests program. The keys and certificates it judges are made in memory against
// the distribution's OpenSSL. No database, configuration, file or network.

#include <cstdio>
#include <stdexcept>

#include <openssl/crypto.h>

#include "../Common/Util/AcmeClient.h"

int
main()
{
   try
   {
      HM::AcmeChecksTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  ACME checks: %s (%s)\n", failure.what(), OpenSSL_version(OPENSSL_VERSION));
      return 1;
   }

   printf("ok    ACME checks: the names a certificate covers, AcmeDomains read, Retry-After in both forms, a rate limit's problem document, the spacing between checks, and a certificate the CA issued refused for another key, a missing name, an expiry or no certificate at all (%s)\n",
      OpenSSL_version(OPENSSL_VERSION));
   return 0;
}
