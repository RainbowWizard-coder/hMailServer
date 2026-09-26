// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "VirusScanningResult.h"

namespace HM
{
   class CustomVirusScanner
   {
   public:
      CustomVirusScanner(void);
      ~CustomVirusScanner(void);

      static VirusScanningResult Scan(const String &executablePath, int virusReturnCode, const String &sFilename);
      static VirusScanningResult Scan(const String &sFilename);

      // The command line the scanner is started with. With %FILE% in the
      // configured one, each %FILE% is replaced by the file's path - in double
      // quotes, unless the configured command line has it inside them already;
      // without, the configured executable and the path, each in double quotes.
      static String BuildCommandLine(const String &executablePath, const String &sFilename);
   };

   // The command lines of the custom scanner and ClamWin, from values: a path
   // with spaces for a %FILE% that is bare, quoted, inside a quoted argument or
   // there twice, and ClamWin's database and temporary directories with and
   // without a trailing backslash. Run by ClassTester; it starts no scanner.
   class ScannerCommandLineTester
   {
   public:
      void Test();
   };

}