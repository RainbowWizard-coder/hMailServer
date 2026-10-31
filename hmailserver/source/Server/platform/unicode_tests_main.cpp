// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The narrow forms of the server's text, as a program of its own for the POSIX
// build - the counterpart of connection-string-tests and legacy-blowfish-tests. On
// Windows both testers run inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches).
//
//   * FormatterTester (Common/Util/Strings/Formatter.h): Formatter's text, and
//     FormatAsAnsi with every count of arguments, each in its place - up to 6.3.7
//     the five-argument form passed the first, the second and the fifth - and
//     Unicode::ToANSI, which FormatAsAnsi ends in: on both platforms a character
//     up to U+00FF as its byte, and nothing at all for a text with one above it.
//     Up to 6.3.7 Linux gave nothing for any character outside ASCII, Windows
//     the bytes above.
//   * SpamAssassinUserTester (Common/AntiSpam/SpamAssassin/SpamAssassinClient.h):
//     the bytes of spamd's User: header for a name, a name within Latin-1, one
//     beyond it and an address held as its UTF-8 bytes each as spamd should read
//     it, and a control character refused - where up to 6.3.7 Linux sent no
//     header for any of the three, so that spamd scanned as its default user.
//
// No configuration, network or spamd. The results do not depend on LANG or the C
// library: GitLab CI's linux-build jobs run this under glibc, the APKBUILD's
// check() under musl. One line a tester, and a non-zero exit on a failure.

#include <cstdio>
#include <stdexcept>

#include "../Common/Util/Strings/Formatter.h"
#include "../Common/AntiSpam/SpamAssassin/SpamAssassinClient.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   int result = 0;

   try
   {
      HM::FormatterTester tester;
      tester.Test();
      printf("ok    Formatter and FormatAsAnsi with one to five arguments, each in its place; Unicode::ToANSI one byte a character up to U+00FF and nothing for a text above it, as on Windows\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      HM::SpamAssassinUserTester tester;
      tester.Test();
      printf("ok    spamd's User: header: a name within Latin-1 and one beyond it in UTF-8, an address held as its bytes as those bytes, a control character refused\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   return result;
}
