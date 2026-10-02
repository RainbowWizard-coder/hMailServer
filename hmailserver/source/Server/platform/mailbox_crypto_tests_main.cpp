// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cryptography of mailbox encryption at rest (Common/Util/MailboxCrypto.h),
// as a program of its own for the POSIX build.
//
// On Windows the same test runs inside the shipping server: ClassTester::
// DoTests calls MailboxCrypto::Test, and the regression suite reaches it through
// Utilities.RunTestSuite. That route does not exist here, so the CMake build
// makes this instead and linux-build.sh runs it with every other *-tests
// program. Until 6.3.6 nothing ran it on Linux at all.
//
// What it holds: X25519 against RFC 7748's vector, HKDF-SHA256 against RFC
// 5869's first case, base64 both ways, every seal and wrap round-tripped with
// its refusals, a message's stage opened and refused - and ForgetOnExit, the
// guard every private key in MailboxKeys.cpp is watched by: the bytes written
// over when the scope ends by a return, early or late, or by an exception
// thrown through it, beside the control of the same exception with nothing
// watching, which leaves them. Nothing here touches the network or a database;
// the wraps take the password-hash work from the defaults, with no
// configuration read.
//
// One line, and a non-zero exit on the first failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/MailboxCrypto.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   try
   {
      HM::MailboxCrypto::Test();
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  mailbox encryption's cryptography: %s\n", failure.what());
      return 1;
   }

   printf("ok    mailbox encryption's cryptography: X25519, HKDF, base64, seals, wraps and stages with their refusals, and a key forgotten on every way out of its scope\n");
   return 0;
}
