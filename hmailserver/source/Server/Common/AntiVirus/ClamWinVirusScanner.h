// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "VirusScanningResult.h"

namespace HM
{
   class ClamWinVirusScanner  
   {
   public:
      ClamWinVirusScanner();
      virtual ~ClamWinVirusScanner();

      static VirusScanningResult Scan(const String &sFilename);
      static VirusScanningResult Scan(const String &scannerExecutable, const String &databasePath, const String &sFilename);

      // clamscan's command line: the executable, --database, the name of the
      // file to scan (it runs in the file's folder) and --tempdir, each quoted.
      static String BuildCommandLine(const String &scannerExecutable, const String &databasePath, const String &sFileToScan, const String &sTempDir);

   protected:
      
   private:


   };
}
