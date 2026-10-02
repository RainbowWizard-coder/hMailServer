// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// DANE as the outbound connection does it (DaneTester, SMTP/TlsPolicy.h) as a
// program of its own for the POSIX build - the counterpart of tls-options-tests.
//
// On Windows DaneTester runs inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches). That route does not exist
// here, so the CMake build makes this and linux-build.sh runs it with every other
// *-tests program. It is where DANE meets the OpenSSL the distribution ships: the
// TLS handshakes it makes run over a BIO pair in memory against certificates it
// makes itself. No database, configuration, file or network.

#include <cstdio>
#include <stdexcept>

#include <openssl/crypto.h>

#include "../SMTP/TlsPolicy.h"

int
main()
{
   try
   {
      HM::DaneTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  DANE: %s (%s)\n", failure.what(), OpenSSL_version(OPENSSL_VERSION));
      return 1;
   }

   printf("ok    DANE: which TLSA records are usable, a validated RRset of none requiring TLS without authentication, DANE-TA authenticating a chain that carries its anchor and refusing another anchor, a missing one, another name and an expired certificate, DANE-EE authenticating its key alone (%s)\n",
      OpenSSL_version(OPENSSL_VERSION));
   return 0;
}
