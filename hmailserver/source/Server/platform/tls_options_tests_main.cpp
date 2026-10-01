// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The TLS contexts' options (SslContextInitializerTester, Common/TCPIP/
// SslContextInitializer.h) as a program of its own for the POSIX build - the
// counterpart of spf-tests and backup-tests.
//
// On Windows the tester runs inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches), where it also reads back
// the mail client's context as the server's own settings set it up. That route
// does not exist here, so the CMake build makes this and linux-build.sh runs it
// with every other *-tests program. It is where the options meet the OpenSSL the
// distribution ships, whatever its version: every combination of the TLS
// versions and the cipher-order settings is set up on a listener's context and on
// a client's, as SslContextInitializer sets them, and SSL_CTX_get_options must
// hold exactly what was asked for - before 6.3.6 it held every option from bit 32
// up as well - and a context set up with the floor alone, as the MTA-STS policy
// fetch and the OAuth2 token request set theirs up, keeps certificate compression
// off as the others do. No database, configuration, network or file.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <stdexcept>

#include <openssl/crypto.h>

#include "../Common/TCPIP/SslContextInitializer.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::SslContextInitializerTester tester;
      tester.Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the TLS contexts' options: %s (%s)\n", failure.what(), OpenSSL_version(OPENSSL_VERSION));
      return 1;
   }

   printf("ok    the TLS contexts' options: every combination of the TLS versions and the cipher-order settings, on a listener's context and a client's, holds exactly what it asked for and nothing from bit 32 up but certificate compression off, which a context at the floor alone keeps off too (%s)\n",
      OpenSSL_version(OPENSSL_VERSION));
   return 0;
}
