// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What the update check keeps of a release's page, as a program of its own for
// the POSIX build.
//
// On Windows UpdateCheckerTester runs inside the server: ClassTester::DoTests
// calls it, and the regression suite reaches it through Utilities.RunTestSuite.
// Nothing reaches ClassTester here, so the CMake build makes this program and
// GitLab CI's linux-build job runs it with the other *-tests programs
// (build/ci/linux-build.sh).
//
// UpdateCheckerTester (Common/Util/UpdateChecker.cpp) reads feed documents
// whose html_url is the project's release page, an https address in capitals,
// the longest address kept and one character longer, and values that are not an
// https address with a host - another scheme, a leading space or tab, a space,
// line break, NUL or DEL inside, no host, a number, none - and requires the
// first kind kept as it is, the rest dropped, and the release read either way.
// No network, no configuration. It throws at its first failed check; this
// prints which, and exits non-zero.
//
// And, in a build with the live update, the systemd-run command the apply
// starts the helper with when the server is itself root
// (UpdateInstaller::SystemdRunCommand): the helper takes the unit to stop and
// the AppImage to verify with and swap from its own command line alone, and
// refuses an AppImage request where that names no image, so the command must
// carry the server's unit and, for an AppImage, the image - each whole, and
// neither when it could not be quoted whole.
//
// And what the apply's watcher makes of the unit the helper runs as, from what
// `systemctl show` prints and through a stand-in systemctl
// (UpdateInstaller::ReadHelperUnitState, AskHelperUnitState): a helper that
// refused the request has ended, and the drain is left at once. And JsonValue's
// integers (JsonValueTester): exact from their digits, held at the end of the
// range asked for when past it.

#include <cstdio>
#include <exception>
#include <fstream>
#include <string>

#include <boost/filesystem.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/JsonDocument.h"
#include "../Common/Util/UpdateChecker.h"
#include "../Common/Util/UpdateInstaller.h"

namespace
{
#if HM_LIVE_UPDATE
   // The checks of SystemdRunCommand; the first that fails is returned.
   std::string CheckSystemdRunCommand()
   {
      using HM::String;
      using HM::UpdateInstaller;
      const String run = _T("/usr/bin/systemd-run");
      const String helper = _T("/var/lib/hmailserver/Updates/hmailserver-update");
      const String request = _T("/var/lib/hmailserver/Updates/apply-6.3.7.request");
      const String updates = _T("/var/lib/hmailserver/Updates");
      const String start = _T("/usr/bin/systemd-run --unit=hmailserver-update-6.3.7 --collect --quiet -- ")
         _T("\"/var/lib/hmailserver/Updates/hmailserver-update\" apply --request ")
         _T("\"/var/lib/hmailserver/Updates/apply-6.3.7.request\" --updates-dir \"/var/lib/hmailserver/Updates\"");

      String appimage = UpdateInstaller::SystemdRunCommand(run, _T("6.3.7"), helper, request, updates,
         _T("hmailserver.service"), _T("/opt/hmailserver/hMailServer.AppImage"));
      if (appimage != start + _T(" --unit \"hmailserver.service\" --image \"/opt/hmailserver/hMailServer.AppImage\""))
         return "an AppImage's command lacks its unit or its image";

      String package = UpdateInstaller::SystemdRunCommand(run, _T("6.3.7"), helper, request, updates,
         _T("hmailserver-mail.service"), _T(""));
      if (package != start + _T(" --unit \"hmailserver-mail.service\""))
         return "a package's command does not carry its unit alone";

      String unquotable = UpdateInstaller::SystemdRunCommand(run, _T("6.3.7"), helper, request, updates,
         _T("hmailserver\".service"), _T("/opt/a\"b/hMailServer.AppImage"));
      if (unquotable != start)
         return "a unit or an image with a double quote in it was put on the command line";

      String none = UpdateInstaller::SystemdRunCommand(run, _T("6.3.7"), helper, request, updates, _T(""), _T(""));
      if (none != start)
         return "an empty unit or image was put on the command line";
      return "";
   }

   // What the apply's watcher makes of the helper's unit: ReadHelperUnitState
   // over what `systemctl show` prints, and AskHelperUnitState through a
   // stand-in systemctl in a directory of this program's own - a refused request
   // (failed, exit status 4) ended, the oneshot at work while it activates, a
   // unit systemd-run's --collect has unloaded ended, no answer unknown, and a
   // unit name that is not one never put on a command line. The watcher leaves
   // the drain once the unit has ended with no outcome written, where until
   // 6.3.6 it waited the ten minutes out.
   std::string CheckHelperUnitState()
   {
      using HM::String;
      using HM::AnsiString;
      using HM::UpdateInstaller;

      struct Case
      {
         const char *text;
         UpdateInstaller::HelperUnitState state;
         int exitStatus;
         const char *what;
      };
      const Case cases[] =
      {
         { "LoadState=loaded\nActiveState=failed\nExecMainStatus=4\n", UpdateInstaller::HelperUnitEnded, 4, "a refused request" },
         { "LoadState=loaded\nActiveState=activating\nExecMainStatus=0\n", UpdateInstaller::HelperUnitAtWork, 0, "the oneshot at work" },
         { "LoadState=loaded\nActiveState=active\nExecMainStatus=0\n", UpdateInstaller::HelperUnitAtWork, 0, "a transient unit at work" },
         { "LoadState=loaded\nActiveState=deactivating\n", UpdateInstaller::HelperUnitAtWork, -1, "a unit stopping" },
         { "LoadState=not-found\nActiveState=inactive\nExecMainStatus=0\n", UpdateInstaller::HelperUnitEnded, 0, "a unit --collect has unloaded" },
         { "LoadState=loaded\r\nActiveState=inactive\r\nExecMainStatus=3\r\n", UpdateInstaller::HelperUnitEnded, 3, "CRLF, a helper that died" },
         { "", UpdateInstaller::HelperUnitUnknown, -1, "no answer" },
         { "Failed to connect to bus: No such file or directory\n", UpdateInstaller::HelperUnitUnknown, -1, "no bus" },
         { "ActiveState=\nExecMainStatus=x4\n", UpdateInstaller::HelperUnitUnknown, -1, "an empty state and a status that is not a number" },
      };
      for (const Case &c : cases)
      {
         int exitStatus = 99;
         if (UpdateInstaller::ReadHelperUnitState(AnsiString(c.text), exitStatus) != c.state || exitStatus != c.exitStatus)
            return std::string("ReadHelperUnitState misread ") + c.what;
      }

      boost::system::error_code error;
      const boost::filesystem::path scratch = boost::filesystem::temp_directory_path(error) /
         ("hm-update-feed-tests-" + std::to_string(static_cast<long long>(::getpid())));
      if (!error)
         boost::filesystem::create_directories(scratch, error);
      if (error)
         return "no scratch directory could be made under the system's temp directory";

      const std::string directory = scratch.string();
      std::string failed;
      {
         // ProcessLauncher reads the temp folder and the external process
         // timeout from the configuration: one of this program's own.
         std::ofstream ini((scratch / "hMailServer.ini").string().c_str(), std::ios::out | std::ios::trunc);
         ini << "[Directories]\nProgramFolder=" << directory << "\nDataFolder=" << directory << "\nLogFolder=" << directory
             << "\nTempFolder=" << directory << "\nEventFolder=" << directory << "\nDatabaseFolder=" << directory << "\n";
      }
      {
         // The stand-in: what it was asked, and the answer for the unit named last.
         std::ofstream fake((scratch / "systemctl").string().c_str(), std::ios::out | std::ios::trunc);
         fake << "#!/bin/sh\n"
              << "echo \"$*\" >> '" << directory << "/asked'\n"
              << "for last; do :; done\n"
              << "case \"$last\" in\n"
              << "   refused.service) printf 'LoadState=loaded\\nActiveState=failed\\nExecMainStatus=4\\n' ;;\n"
              << "   working.service) printf 'LoadState=loaded\\nActiveState=activating\\nExecMainStatus=0\\n' ;;\n"
              << "   *) exit 4 ;;\n"
              << "esac\n";
      }
      ::chmod((scratch / "systemctl").string().c_str(), 0755);

      // A temp directory mounted noexec - which a package builder's may be - runs
      // no stand-in; the readings above are then all this checks, and it says so.
      if (::access((scratch / "systemctl").string().c_str(), X_OK) != 0)
      {
         std::printf("note  UpdateInstaller's helper unit: %s does not allow running a program, so the stand-in systemctl was not asked\n", directory.c_str());
         boost::filesystem::remove_all(scratch, error);
         return "";
      }

      try
      {
         HM::IniFileSettings::Instance()->SetInitializationFile(String((scratch / "hMailServer.ini").string().c_str()));
         HM::IniFileSettings::Instance()->LoadSettings();

         const String systemctl = String((scratch / "systemctl").string().c_str());
         const String answer = String((scratch / "answer.txt").string().c_str());
         int exitStatus = -1;

         if (UpdateInstaller::AskHelperUnitState(systemctl, _T("refused.service"), answer, exitStatus) != UpdateInstaller::HelperUnitEnded || exitStatus != 4)
            failed = "a refused request's unit, asked of systemctl, was not read as ended with exit status 4";
         else if (UpdateInstaller::AskHelperUnitState(systemctl, _T("working.service"), answer, exitStatus) != UpdateInstaller::HelperUnitAtWork)
            failed = "a helper at work, asked of systemctl, was not read as at work";
         else if (UpdateInstaller::AskHelperUnitState(systemctl, _T("missing.service"), answer, exitStatus) != UpdateInstaller::HelperUnitUnknown)
            failed = "systemctl's refusal was read as a state";
         else if (UpdateInstaller::AskHelperUnitState(String((scratch / "no-such-systemctl").string().c_str()), _T("refused.service"), answer, exitStatus) != UpdateInstaller::HelperUnitUnknown)
            failed = "a systemctl that is not there was read as an answer";
         else if (UpdateInstaller::AskHelperUnitState(systemctl, _T("x.service; touch pwned"), answer, exitStatus) != UpdateInstaller::HelperUnitUnknown)
            failed = "a unit name that is not one was asked about";
         else if (boost::filesystem::exists(scratch / "answer.txt"))
            failed = "the answer file was left behind";
         else
         {
            std::ifstream asked((scratch / "asked").string().c_str());
            std::string line;
            int lines = 0;
            while (std::getline(asked, line))
            {
               lines++;
               if (line.find("show --property=LoadState --property=ActiveState --property=ExecMainStatus ") != 0)
                  failed = "systemctl was asked otherwise than show with the three properties: " + line;
            }
            if (failed.empty() && lines != 3)
               failed = "systemctl was asked " + std::to_string(lines) + " times, not 3: a name that is not a unit's reached it";
         }
      }
      catch (const std::exception &e)
      {
         failed = std::string("the configuration could not be loaded: ") + e.what();
      }

      boost::filesystem::remove_all(scratch, error);
      return failed;
   }
#endif
}

int main()
{
   try
   {
      HM::UpdateCheckerTester tester;
      tester.Test();
      std::printf("ok    UpdateCheckerTester\n");
   }
   catch (const std::exception &e)
   {
      std::printf("FAIL  UpdateCheckerTester: %s\n", e.what());
      return 1;
   }
   try
   {
      HM::JsonValueTester tester;
      tester.Test();
      std::printf("ok    JsonValueTester: a number as an integer, exact or held at the end of its range\n");
   }
   catch (const std::exception &e)
   {
      std::printf("FAIL  JsonValueTester: %s\n", e.what());
      return 1;
   }
#if HM_LIVE_UPDATE
   std::string failed = CheckSystemdRunCommand();
   if (!failed.empty())
   {
      std::printf("FAIL  UpdateInstaller::SystemdRunCommand: %s\n", failed.c_str());
      return 1;
   }
   std::printf("ok    UpdateInstaller::SystemdRunCommand\n");

   failed = CheckHelperUnitState();
   if (!failed.empty())
   {
      std::printf("FAIL  UpdateInstaller's helper unit: %s\n", failed.c_str());
      return 1;
   }
   std::printf("ok    UpdateInstaller's helper unit: a refused request's unit read as ended, one at work as at work, and nothing but a unit's name asked about\n");
#endif
   return 0;
}
