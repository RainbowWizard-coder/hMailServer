// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The live update's trust (IniSettingStore::IsTrustAnchorName) held to the rule
// that it is read from hMailServer.INI alone, as a program of its own for the
// POSIX build - the counterpart of spf-tests and backup-tests.
//
// On Windows IniFileSettingsTester::TestTrustAnchors runs inside the service with
// the other class tests (ClassTester, which Utilities.RunTestSuite reaches). That
// route does not exist here, so the CMake build makes this and linux-build.sh
// runs it with every other *-tests program. It is the platform where the rule has
// a second reader: hmailserver-update runs `hmailserver --verify-update` as root,
// which opens no database and reads this file, and the server has to read the
// same five values the same way or the two checks are not one check - the first
// real Linux live update, on 29 September 2026, found a signer stored over REST
// used by the server and not by root.
//
// It writes a configuration file of its own in a directory of its own under the
// system's temp directory - a signer of a site's own, as UpdateFeed.md describes
// one - loads it as the server and --verify-update load theirs, and then, with a
// database overlay holding other values for all five, requires the server to read
// the file's, the trust the verifier builds with the overlay and without it to be
// the same, and an administrator's change to each to be refused while the file's
// own value is let through. No database and no network; the directory is removed
// again.
//
// It prints one line either way and exits non-zero on the first failure.

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

#include <boost/filesystem.hpp>

#include "../Common/Application/IniFileSettings.h"

int
main(int /*argc*/, char * /*argv*/[])
{
   // A directory of its own, which the configuration names for every directory
   // it has, so that nothing the settings loader or a report might write lands
   // anywhere else; removed again at the end.
   boost::system::error_code error;
   const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
      ("hm-trust-anchor-tests-" + std::to_string((long long) ::getpid()));
   if (!error)
      boost::filesystem::create_directories(scratch, error);

   if (error)
   {
      printf("FAIL  the live update's trust: no scratch directory could be made under the system's temp directory\n");
      return 1;
   }

   const boost::filesystem::path file = scratch / "hMailServer.ini";
   const std::string directory = scratch.string();

   {
      std::ofstream ini(file.string().c_str(), std::ios::out | std::ios::trunc);
      ini << "[Directories]\n"
          << "ProgramFolder=" << directory << "\n"
          << "DataFolder=" << directory << "\n"
          << "LogFolder=" << directory << "\n"
          << "TempFolder=" << directory << "\n"
          << "EventFolder=" << directory << "\n"
          << "DatabaseFolder=" << directory << "\n"
          << "\n"
          << "[Settings]\n"
          << "UpdateSigningIdentity=https://gitlab.example.net/mail/hmailserver//.gitlab-ci.yml@refs/tags/v\n"
          << "UpdateSigningIssuer=https://gitlab.example.net\n"
          << "UpdateSourceRepository=https://gitlab.example.net/mail/hmailserver\n"
          << "UpdateChannel=stable\n";
      if (!ini)
      {
         printf("FAIL  the live update's trust: %s could not be written\n", file.string().c_str());
         boost::filesystem::remove_all(scratch, error);
         return 1;
      }
   }

   int result = 0;

   try
   {
      HM::IniFileSettings::Instance()->SetInitializationFile(HM::String(file.string().c_str()));
      HM::IniFileSettings::Instance()->LoadSettings();

      HM::IniFileSettingsTester tester;
      tester.TestTrustAnchors();

      printf("ok    the live update's trust: read from the file alone, the same trust with a database overlay and without one, an administrator's change refused\n");
   }
   catch (const std::exception &failure)
   {
      printf("FAIL  the live update's trust: %s\n", failure.what());
      result = 1;
   }

   boost::filesystem::remove_all(scratch, error);
   return result;
}
