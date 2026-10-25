// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
#include "ClamWinVirusScanner.h"

#include "../Util/ProcessLauncher.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A directory for an argument in double quotes: without a trailing
      // backslash, which the C runtime reads as escaping the closing quote -
      // --database="C:\ClamWin\db\" gave clamscan the quote and everything
      // after it, the file to scan and --tempdir, as the database path. A
      // drive's root keeps a name of its own: "D:" alone is the drive's
      // current directory.
      String QuotableDirectory(const String &directory)
      {
         String result = directory;

         while (result.GetLength() > 1 && result.Right(1) == _T("\\"))
            result = result.Left(result.GetLength() - 1);

         if (result.GetLength() == 2 && result.GetAt(1) == _T(':'))
            result += _T("\\.");

         return result;
      }
   }

   ClamWinVirusScanner::ClamWinVirusScanner()
   {

   }

   ClamWinVirusScanner::~ClamWinVirusScanner()
   {

   }

   VirusScanningResult 
   ClamWinVirusScanner::Scan(const String &sFilename)
   {
      String scannerExecutable = Configuration::Instance()->GetAntiVirusConfiguration().ClamWinExecutable();
      String databasePath = Configuration::Instance()->GetAntiVirusConfiguration().ClamWinDatabase();

      return Scan(scannerExecutable, databasePath, sFilename);
   }

   VirusScanningResult 
   ClamWinVirusScanner::Scan(const String &scannerExecutable, const String &databasePath, const String &sFilename)
   {
      LOG_DEBUG("Running ClamWin");

      String sPath = FileUtilities::GetFilePath(sFilename);
      String sFileToScan = FileUtilities::GetFileNameFromFullPath(sFilename);
      String sTempDir = IniFileSettings::Instance()->GetTempDirectory();

      String sCommandLine = BuildCommandLine(scannerExecutable, databasePath, sFileToScan, sTempDir);

      unsigned int exitCode = 0;
      ProcessLauncher launcher(sCommandLine, sPath);
      launcher.SetErrorLogTimeout(20000);

      ProcessLauncher::FailureReason failureReason = ProcessLauncher::FailureReason::None;
      if (!launcher.Launch(exitCode, failureReason))
      {
         // A timeout is the scanner being slow, not the path being wrong - see
         // CustomVirusScanner for the full reasoning; both scanners had the
         // same misleading message.
         if (failureReason == ProcessLauncher::FailureReason::TimedOut)
            return VirusScanningResult("ClamWinVirusScanner::Scan",
               Formatter::Format("ClamWin did not finish within the maximum time (ExternalProcessTimeout) and was terminated. Command line: {0}.", sCommandLine));

         return VirusScanningResult("ClamWinVirusScanner::Scan",
            Formatter::Format("Unable to launch ClamWin. Check that the executable path is correct and runnable by the service account. Command line: {0}.", sCommandLine));
      }

      String sDebugMessage = Formatter::Format("ClamWin: {0}. Return code: {1}", sCommandLine, exitCode);
      LOG_DEBUG(sDebugMessage);

      if (exitCode == 1)
         return VirusScanningResult(VirusScanningResult::VirusFound, "Unknown");
      else
         return VirusScanningResult(VirusScanningResult::NoVirusFound, Formatter::Format("Return code: {0}", exitCode));

   }

   String
   ClamWinVirusScanner::BuildCommandLine(const String &scannerExecutable, const String &databasePath, const String &sFileToScan, const String &sTempDir)
   {
      // Quote the executable path so a path containing spaces cannot be
      // misinterpreted by CreateProcess (which searches NULL lpApplicationName
      // by splitting on whitespace, e.g. C:\Program.exe before C:\Program Files\...).
      String sExecutable = scannerExecutable;
      if (sExecutable.GetLength() > 0 && sExecutable.GetAt(0) != _T('"'))
         sExecutable = _T("\"") + sExecutable + _T("\"");

      const String database = QuotableDirectory(databasePath);
      const String tempDirectory = QuotableDirectory(sTempDir);

      String sCommandLine;
      sCommandLine.Format(_T("%s --database=\"%s\" \"%s\" --tempdir=\"%s\""), sExecutable.c_str(), database.c_str(), sFileToScan.c_str(), tempDirectory.c_str());
      return sCommandLine;
   }
}