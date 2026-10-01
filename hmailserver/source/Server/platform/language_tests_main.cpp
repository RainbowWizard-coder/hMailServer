// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// COM's Language objects (Common/Util/Language.h, Languages.h) held to the
// Languages directory they were made for, as a program of its own for the POSIX
// build - the counterpart of spf-tests and backup-tests. Nothing in the Linux
// server reads a Language (COM is not built here), but the units are the same, and
// this is where a ProgramFolder can be changed under a loaded set without a
// service to disturb.
//
// Two checks. LanguageTester, which ClassTester runs inside the Windows service:
// a language made for a directory reads its translation there. Then the case the
// directory is kept for: a configuration file of its own names a program folder,
// Languages::Load reads it, one of its languages is taken and not yet used, the
// file is changed to name another program folder - whose English numbers the same
// words the other way round - and read again as a reinitialise reads it; the
// language taken first must still read the first folder's translation, through
// the first folder's English, and a load made now the second's. Up to 6.3.5 the
// first language read the second folder's file through the first folder's
// English.
//
// Scratch files under the system's temp directory, removed again; no database and
// no network. One line per check, and a non-zero exit on a failure.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/Language.h"
#include "../Common/Util/Languages.h"

namespace
{
   bool WriteIni(const boost::filesystem::path &file, const std::string &programFolder, const std::string &scratch)
   {
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << programFolder << "\n"
          << "DataFolder=" << scratch << "\n"
          << "LogFolder=" << scratch << "\n"
          << "TempFolder=" << scratch << "\n"
          << "EventFolder=" << scratch << "\n"
          << "DatabaseFolder=" << scratch << "\n"
          << "\n"
          << "[GUILanguages]\n"
          << "ValidLanguages=english,xx\n";
      return (bool) ini;
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-language-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);
   if (error)
   {
      printf("FAIL  Language: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   int result = 0;

   try
   {
      HM::LanguageTester tester;
      tester.Test();
      printf("ok    Language: a language reads its translation from the directory it was made for\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   try
   {
      const boost::filesystem::path first = scratch / "first";
      const boost::filesystem::path second = scratch / "second";
      HM::LanguageTester::WriteLanguages(HM::String((first / "Languages").wstring()), false, "Arkiv-first", "Avsluta-first");
      HM::LanguageTester::WriteLanguages(HM::String((second / "Languages").wstring()), true, "Avsluta-second", "Arkiv-second");

      const boost::filesystem::path file = scratch / "hMailServer.ini";
      if (!WriteIni(file, first.string(), scratch.string()))
         throw std::logic_error("Language: the configuration file could not be written");

      HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
      HM::IniFileSettings::Instance()->LoadSettings();

      HM::Languages::Instance()->Load();
      std::shared_ptr<HM::Language> taken = HM::Languages::Instance()->GetLanguage(_T("xx"));
      if (!taken)
         throw std::logic_error("Language: the loaded set has no language xx");

      // The program folder changed and the file read again, before the language
      // taken is first used.
      if (!WriteIni(file, second.string(), scratch.string()))
         throw std::logic_error("Language: the configuration file could not be written again");
      HM::IniFileSettings::Instance()->LoadSettings();
      if (HM::IniFileSettings::Instance()->GetLanguageDirectory().Find(_T("second")) < 0)
         throw std::logic_error("Language: the configuration read again does not name the second program folder");

      const HM::String file1 = taken->GetString(_T("File"));
      const HM::String exit1 = taken->GetString(_T("Exit"));
      if (file1 != _T("Arkiv-first") || exit1 != _T("Avsluta-first"))
         throw std::logic_error(std::string("Language: a language loaded before ProgramFolder changed read ") +
            HM::AnsiString(file1).c_str() + " and " + HM::AnsiString(exit1).c_str() + ", not the first folder's Arkiv-first and Avsluta-first");

      // A load made now is the second folder's, through its own English.
      HM::Languages::Instance()->Load();
      std::shared_ptr<HM::Language> fresh = HM::Languages::Instance()->GetLanguage(_T("xx"));
      if (!fresh || fresh->GetString(_T("File")) != _T("Arkiv-second") || fresh->GetString(_T("Exit")) != _T("Avsluta-second"))
         throw std::logic_error("Language: a load made after ProgramFolder changed is not the second folder's");

      printf("ok    Language: one taken before ProgramFolder changed keeps its folder, and a load made after takes the new one\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  %s\n", failure.what());
      result = 1;
   }

   boost::filesystem::remove_all(scratch, error);
   return result;
}
