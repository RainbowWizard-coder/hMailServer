// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "CustomVirusScanner.h"
#include "ClamWinVirusScanner.h"
#include "../../SMTP/SMTPConfiguration.h"
#include "../Util/ProcessLauncher.h"

#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   CustomVirusScanner::CustomVirusScanner(void)
   {
   }

   CustomVirusScanner::~CustomVirusScanner(void)
   {

   }
   
   VirusScanningResult 
   CustomVirusScanner::Scan(const String &sFilename)
   {
      AntiVirusConfiguration &pConfig = Configuration::Instance()->GetAntiVirusConfiguration();
      String executablePath = pConfig.GetCustomScannerExecutable();
      int virusReturnCode = pConfig.GetCustomScannerReturnValue();
      return Scan(executablePath, virusReturnCode, sFilename);
   }

   VirusScanningResult 
   CustomVirusScanner::Scan(const String &executablePath, int virusReturnCode, const String &sFilename)
   {
      LOG_DEBUG("Running custom virus scanner...");


      String sPath = FileUtilities::GetFilePath(sFilename);

      String sCommandLine = BuildCommandLine(executablePath, sFilename);

      unsigned int exitCode = 0;
      ProcessLauncher launcher(sCommandLine, sPath);
      launcher.SetErrorLogTimeout(20000);

      ProcessLauncher::FailureReason failureReason = ProcessLauncher::FailureReason::None;
      if (!launcher.Launch(exitCode, failureReason))
      {
         // The two failures read oppositely. A timeout means the scanner was
         // reachable and simply too slow - which is exactly what AVFailAction
         // exists to arbitrate - so reporting it as a launch failure would send
         // an administrator to check a path that is perfectly correct.
         if (failureReason == ProcessLauncher::FailureReason::TimedOut)
            return VirusScanningResult("CustomVirusScanner::Scan",
               Formatter::Format("The virus scanner did not finish within the maximum time (ExternalProcessTimeout) and was terminated. Command line: {0}.", sCommandLine));

         return VirusScanningResult("CustomVirusScanner::Scan",
            Formatter::Format("Unable to launch the virus scanner. Check that the executable path is correct and runnable by the service account. Command line: {0}.", sCommandLine));
      }

      String sDebugMessage = Formatter::Format("Scanner: {0}. Return code: {1}", sCommandLine, exitCode);
      LOG_DEBUG(sDebugMessage);

      if (exitCode == virusReturnCode)
         return VirusScanningResult(VirusScanningResult::VirusFound, "Unknown");
      else
         return VirusScanningResult(VirusScanningResult::NoVirusFound, Formatter::Format("Return code: {0}", exitCode));

   }

   String
   CustomVirusScanner::BuildCommandLine(const String &executablePath, const String &sFilename)
   {
      const String placeholder = _T("%FILE%");

      if (executablePath.Find(placeholder) < 0)
      {
         // Quote both the executable and the file path so that spaces in either
         // cannot be misinterpreted by CreateProcess (unquoted-path hijack).
         String sExe = executablePath;
         if (sExe.GetLength() > 0 && sExe.GetAt(0) != _T('"'))
            sExe = _T("\"") + sExe + _T("\"");

         String sCommandLine;
         sCommandLine.Format(_T("%s \"%s\""), sExe.c_str(), sFilename.c_str());
         return sCommandLine;
      }

      // Each %FILE% becomes the path. Until 26 September 2026 the path went in
      // as it was, so a command line with a bare %FILE% handed the scanner the
      // path cut at its first space - and the default data directory, under
      // C:\Program Files, has one: the scanner scanned no file, answered with
      // an exit code other than the virus one, and the message went on as
      // clean. A %FILE% the command line already has inside double quotes -
      // "%FILE%", as the documented examples write it, or "--file=%FILE%" - is
      // replaced by the path alone, any other by the path in double quotes.
      //
      // Every double quote turns quoting on or off, which is how ProcessLauncher
      // splits a command line on Linux and how the C runtime splits one on
      // Windows unless a backslash escapes the quote - nothing a scanner's
      // command line needs to do.
      String sCommandLine;
      bool quoted = false;
      const int length = executablePath.GetLength();
      const int placeholderLength = placeholder.GetLength();

      for (int i = 0; i < length; )
      {
         if (executablePath.Mid(i, placeholderLength) == placeholder)
         {
            if (quoted)
               sCommandLine += sFilename;
            else
               sCommandLine += _T("\"") + sFilename + _T("\"");

            i += placeholderLength;
            continue;
         }

         const TCHAR character = executablePath.GetAt(i);

         if (character == _T('"'))
            quoted = !quoted;

         sCommandLine += character;
         i++;
      }

      return sCommandLine;
   }

   namespace
   {
      void ExpectCommandLine(const String &actual, const String &expected, const std::string &what)
      {
         if (actual != expected)
            throw std::logic_error("ScannerCommandLineTester: " + what + ": " + std::string(AnsiString(actual)));
      }
   }

   void
   ScannerCommandLineTester::Test()
   {
      // The queue file under the default data directory, which has a space.
      const String file = _T("C:\\Program Files\\hMailServer\\Data\\{0B7A2C1D-9E4F-4A3B-8C6D-5E7F8091A2B3}.eml");
      const String quotedFile = _T("\"") + file + _T("\"");

      // A bare %FILE% is quoted: the path reached the scanner cut at the space.
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(_T("C:\\Scanner\\scan.exe %FILE%"), file),
         _T("C:\\Scanner\\scan.exe ") + quotedFile, "a bare %FILE%");

      // One already in quotes is not quoted again: the documented form.
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(
            _T("\"C:\\Program Files\\Windows Defender\\MpCmdRun.exe\" -Scan -ScanType 3 -File \"%FILE%\" -DisableRemediation"), file),
         _T("\"C:\\Program Files\\Windows Defender\\MpCmdRun.exe\" -Scan -ScanType 3 -File ") + quotedFile + _T(" -DisableRemediation"),
         "a quoted %FILE%");

      // Nor one inside a quoted argument.
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(_T("scan.exe \"--file=%FILE%\" --report"), file),
         _T("scan.exe \"--file=") + file + _T("\" --report"), "%FILE% inside a quoted argument");

      // Each of two is judged on its own.
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(_T("scan.exe --in=%FILE% --log=\"%FILE%.log\""), file),
         _T("scan.exe --in=") + quotedFile + _T(" --log=\"") + file + _T(".log\""), "%FILE% twice");

      // Without %FILE%: the executable and the path, each quoted, as before.
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(_T("C:\\Scanner\\scan.exe"), file),
         _T("\"C:\\Scanner\\scan.exe\" ") + quotedFile, "no %FILE%");
      ExpectCommandLine(CustomVirusScanner::BuildCommandLine(_T("\"C:\\Program Files\\Scanner\\scan.exe\""), file),
         _T("\"C:\\Program Files\\Scanner\\scan.exe\" ") + quotedFile, "no %FILE%, the executable quoted");

      // ClamWin: a trailing backslash on a directory would escape its closing
      // quote, and a drive's root keeps a name of its own.
      const String clamscan = _T("C:\\Program Files\\ClamWin\\bin\\clamscan.exe");
      const String expected = _T("\"C:\\Program Files\\ClamWin\\bin\\clamscan.exe\" --database=\"C:\\ProgramData\\.clamwin\\db\" \"x.eml\" --tempdir=\"C:\\Program Files\\hMailServer\\Temp\"");

      ExpectCommandLine(ClamWinVirusScanner::BuildCommandLine(clamscan, _T("C:\\ProgramData\\.clamwin\\db"), _T("x.eml"), _T("C:\\Program Files\\hMailServer\\Temp")),
         expected, "ClamWin");
      ExpectCommandLine(ClamWinVirusScanner::BuildCommandLine(clamscan, _T("C:\\ProgramData\\.clamwin\\db\\"), _T("x.eml"), _T("C:\\Program Files\\hMailServer\\Temp\\")),
         expected, "ClamWin, both directories with a trailing backslash");
      ExpectCommandLine(ClamWinVirusScanner::BuildCommandLine(_T("\"") + clamscan + _T("\""), _T("D:\\"), _T("x.eml"), _T("E:\\")),
         _T("\"C:\\Program Files\\ClamWin\\bin\\clamscan.exe\" --database=\"D:\\.\" \"x.eml\" --tempdir=\"E:\\.\""),
         "ClamWin, drive roots");
   }
}