// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifndef HM_PLATFORM_POSIX

#include <string>
#include <vector>

namespace HM
{
   // The checks an administrator's install of an update makes before anything
   // runs elevated (AdminUpdateChecks.h, AuthenticodePublisher.h, LoweredChild.h),
   // each with a case it must refuse beside the case it must pass:
   //
   //   - the patterns: the work id, the version (6.3.7 & calc refused), the token
   //     (63 and 65 characters, upper case, not hex);
   //   - the permission rule on descriptors, the cases of
   //     build/testdata/install-permissions-cases.json in the same order;
   //   - the work folder: made with its owner and its protected DACL, a second
   //     one of the same name refused, one already there as a junction refused,
   //     a held folder that cannot be renamed, a file made new twice refused;
   //   - the safe copy: a file copied byte for byte without its alternate
   //     stream; a folder, a file over the cap, a junction on the way and a
   //     symbolic link (where one can be made) refused;
   //   - the architecture marker and the version order;
   //   - Authenticode and the publisher pin against the real installers: 6.3.1
   //     (its leaf expired, the timestamp carries it), 6.3.3, 6.3.4 and 6.3.5
   //     pass; 6.3.0 is not signed; 6.3.5 with one byte of its setup data changed
   //     is refused for its digest, cut short is refused, and with bytes appended
   //     is refused or - where Windows ignores the tail - has another SHA-256
   //     than the release, which the Sigstore digest refuses; the .NET Desktop
   //     Runtime installer (Microsoft's, validly signed) is refused as another
   //     publisher; a catalog-signed Windows file is refused as unsigned; the
   //     site-signer pin passes a subject equal to the installed program's
   //     through the same root, refuses another subject, the same subject held
   //     to another root, and an unsigned installed program; the pin alone
   //     refuses another chain, an extra subject attribute, no code-signing EKU
   //     and no timestamp; a name with a NUL in a value is not read at all; a
   //     revocation check given no time is refused as timed out, not as revoked;
   //   - the lowered child: the groups UAC filters told from those it leaves;
   //     its token read back while it is suspended - Administrators deny-only,
   //     no group UAC filters enabled, no privilege enabled but SeChangeNotify,
   //     integrity Low or lower - its exit code carried back, a second process
   //     refused by its job, and one that does not answer in time killed and
   //     reported so.
   //
   // The installers are test data, not in the repository: the folder
   // HMAILSERVER_INSTALLER_FIXTURES names, or C:\HMTest\Installers, filled by
   // build/fetch-installer-fixtures.ps1, which fetches each by its SHA-256. A
   // missing one FAILS this tester, naming the script: a check whose test never
   // runs looks exactly like one that does not work. The sentence names the
   // computer the tester ran on and Windows' error, and says "missing" only
   // where Windows says there is no such file or path.
   //
   // Win32 and the C++ library only, so that the same file runs in the internal
   // suite (Utilities.RunTestSuite, as SYSTEM) and alone; a failure throws
   // std::logic_error with the sentence.
   class AdminUpdateTester
   {
   public:
      // allowUnelevated: the parts that need an administrator's token (making a
      // folder owned by the Administrators) are said to be skipped rather than
      // failed - for a run by hand at an unelevated prompt only. The internal
      // suite runs as SYSTEM and never passes it.
      explicit AdminUpdateTester(bool allowUnelevated = false);

      void Test();

      // What was skipped, for a run by hand to print.
      const std::vector<std::wstring> &Skipped() const { return skipped_; }

      // The fixtures folder the tester reads.
      static std::wstring FixturesFolder();

      // Empty when the fixture at path is there; otherwise the sentence a
      // failure gives - missing, with the script to run, where Windows answers
      // that the file or the path is not found, and could not be read where it
      // answers anything else - with the computer's name and Windows' error, its
      // number and its text. AdminUpdateStageTester asks it too.
      static std::wstring FixtureProblem(const std::wstring &path);

   private:
      void TestPatterns_();
      void TestPermissionRule_();
      void TestWorkFolder_();
      void TestSafeCopy_();
      void TestArchitectureAndVersions_();
      void TestAuthenticode_();
      void TestPinAlone_();
      void TestLoweredChild_();

      bool allowUnelevated_;
      bool elevated_;
      std::wstring scratch_;
      std::vector<std::wstring> skipped_;
   };
}

#endif
