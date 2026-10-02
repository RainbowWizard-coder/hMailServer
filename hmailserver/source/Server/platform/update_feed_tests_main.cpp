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

#include <cstdio>
#include <exception>
#include <string>

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
#if HM_LIVE_UPDATE
   std::string failed = CheckSystemdRunCommand();
   if (!failed.empty())
   {
      std::printf("FAIL  UpdateInstaller::SystemdRunCommand: %s\n", failed.c_str());
      return 1;
   }
   std::printf("ok    UpdateInstaller::SystemdRunCommand\n");
#endif
   return 0;
}
