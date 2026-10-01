// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// An administrator's install of an update, on a server whose service cannot run
// an installer (docs/ServiceAccount.md, "The live update").
//
// A service that runs as NT SERVICE\hMailServer checks for updates and downloads
// and verifies them, and cannot install one: the installer needs an
// administrator. The Control Panel on the server - run by an administrator, the
// one person already trusted to install software there - installs it instead,
// after one click and Windows' own elevation prompt:
//
//   1. The Control Panel checks that nobody but SYSTEM, the Administrators and
//      TrustedInstaller can change what it is about to elevate
//      (Services/InstallationPermissions.cs), then asks Windows to run
//      {app}\Bin\hMailServer.exe /InstallUpdate elevated (ShellExecuteEx
//      "runas"). Nothing the service does, says or writes can start this: only
//      a person's click at a Control Panel on the server, and that person's
//      answer to Windows' prompt.
//
//   2. THE STAGE, RunInstallUpdateCommand, elevated. It re-derives everything
//      and trusts nothing the service wrote:
//        - its own path must be the one the service manager has for the
//          service, in a folder named Bin; {app} is the folder above;
//        - the permission rule again (AdminUpdateChecks::CheckInstallation),
//          over {app}, Bin, ControlPanel, hMailServer.INI and the Sigstore trust
//          files the INI names;
//        - a work folder of its own, {app}\AdminUpdate\<id>, made new with a
//          protected DACL of SYSTEM and the Administrators alone, and held;
//        - the service's download copied into it (AdminUpdateChecks::SafeCopy:
//          no link followed, none on the way, the main stream only) and held
//          shared for reading only from then until it has run;
//        - on the held copy: Authenticode with the publisher pinned
//          (AuthenticodePublisher.h), the version resource equal to the release
//          offered and its architecture this build's, the release newer than
//          this binary's own HMAILSERVER_VERSION, its SHA-256, and its Sigstore
//          bundle - parsed not here but in hMailServer.exe --verify-bundle, a
//          child with the administrator's rights taken away (LoweredChild.h),
//          whose answer the Authenticode pin does not depend on;
//        - then it waits while the Control Panel asks the service to take the
//          backup and fetch the rollback image (COM), checks the rollback image
//          the same way, copies hMailServer.Updater.exe beside them and hands
//          over to it.
//      It writes a log line nowhere but its work folder, and before anything
//      else it forbids every other write of the logger and of ErrorManager in
//      this process (Logger::ForbidWritesForThisProcess): R3 of the design,
//      nothing elevated writes where the service can.
//
//   3. THE UPDATER, hMailServer.Updater.exe --mode administrator, from the work
//      folder: it waits for the Control Panel to close with the exit code that
//      means go, runs the installer it holds by its SHA-256, runs the rollback
//      image if the service does not come back, writes the outcome into
//      administrator-only files, and starts the Control Panel again, unelevated,
//      as the user who clicked.
//
// hMailServer.exe dispatches both verbs first thing in _tWinMain, before the
// crash oracle, COM and anything that reads the configuration.
#ifndef HM_PLATFORM_POSIX

#include "SigstoreVerifier.h"

#include <string>
#include <vector>

namespace HM
{
   class AdminUpdate
   {
   public:
      // The stage's exit codes, which the Control Panel turns into sentences
      // (Services/AdministratorUpdate.cs holds the same numbers).
      enum StageExit
      {
         StageHandedOver = 0,       // the Updater is running
         StageArguments = 2,        // a switch unknown or malformed, or this is not the service's own binary in Bin
         StageNotElevated = 3,
         StagePermissions = 4,      // the permission rule failed; status.txt names the object when it could be written
         StageBusy = 5,             // another administrator's install is under way
         StageRefused = 6,          // the installer did not pass a check; status.txt says which
         StageParent = 9,           // not started by the Control Panel it names
         StageTrustFile = 10,       // a trust file hMailServer.INI names fails the permission rule
         StageDiskSpace = 11,
         StageNameTaken = 12,       // the work folder or an event of that name was already there
         StageCancelled = 13,       // the Control Panel cancelled or ended before the service was ready
         StageFailed = 14           // anything else; status.txt says what
      };

      // hMailServer.exe --update-version <v> --update-work <id> --control-panel <pid>
      // [--update-wait <seconds>] /InstallUpdate
      static int RunInstallUpdateCommand();

      // hMailServer.exe --verify-bundle: reads one request (MakeVerifyRequest) from
      // its standard input and answers on its standard output
      //   verified <identity> <integrated time>
      // with exit code 0, or
      //   refused <reason>
      // with 1 (the bundle does not verify) or 2 (the request is malformed).
      // It reads and writes nothing else.
      static int RunVerifyBundleChild();

      // Whether the command line is one of the two verbs; the dispatch in
      // _tWinMain asks this before anything else.
      static bool IsInstallUpdateCommandLine();
      static bool IsVerifyBundleCommandLine();

      // The child's request: the release, the SHA-256 the stage read from its
      // held copy, the trust as the stage configured it, and the bundle's bytes.
      // A frame of length-prefixed fields; nothing optional, nothing trailing.
      static std::string MakeVerifyRequest(const String &version, const std::vector<unsigned char> &sha256, const SigstoreTrust &trust,
                                           const AnsiString &bundle);
      static bool ParseVerifyRequest(const std::string &request, String &version, std::vector<unsigned char> &sha256, SigstoreTrust &trust,
                                     AnsiString &bundle, std::string &why);

      // The child's answer, as the stage reads it: true only for exit 0 and one
      // well-formed "verified" line. identity and integratedTime from it.
      static bool ReadVerifyAnswer(unsigned long exitCode, const std::string &output, std::string &identity, std::string &integratedTime,
                                   std::string &why);

      // The installation's AdminUpdate folder and the two files in it the
      // service reads: the last administrator's install's outcome, and the mark
      // of one in progress. From the service's own binary's location, never
      // from the INI or the database.
      static String AdminUpdateFolder();
      static String LastInstallPath();
      static String InProgressPath();

      // Updates\last-admin-install.reported, where the service records the id of
      // the last administrator's install it reported.
      static String ReportedIdPath();

      // The go code the Control Panel exits with so that the Updater installs:
      // "HMUP". Anything else is a cancellation.
      static const unsigned long GoExitCode = 0x484D5550;
   };

   // In the internal suite (Utilities.RunTestSuite): the child's request made and
   // read back, and every malformed form of it refused - cut short, a field
   // longer than the frame, a count past the limit, bytes after the end, a
   // digest of 31 bytes, another magic; the answer read only from exit 0 and one
   // well-formed line; VerifyDigest against the real 6.3.5 installer's bundle
   // (a fixture, AdminUpdateTester::FixturesFolder), the same verdict as
   // VerifyFile, and a digest one bit different refused; and hMailServer.exe
   // --verify-bundle itself, run lowered by LoweredChild, verifying that bundle,
   // refusing the digest one bit off, refusing a malformed request with exit 2,
   // and killed and not believed when it is given no time.
   class AdminUpdateStageTester
   {
   public:
      void Test();
   };
}

#endif
