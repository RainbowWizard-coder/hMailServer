// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // The third of the four parts of the roadmap's live update: the apply.
   //
   // The service cannot run its own installer, because the installer stops the
   // service. So Apply verifies the downloaded installer once more, fetches and
   // verifies the running version's installer as the rollback image where the feed
   // still offers it, issues the single-use token the database upgrade will present
   // (UpdateApplyToken), copies hMailServer.Updater.exe out of Bin - which the
   // installer is about to replace - into the Updates folder, and starts it there,
   // detached, with the SHA-256 of the installer and of the rollback image as they
   // were checked. The Updates folder admits changes from SYSTEM, the
   // Administrators and the service alone, and the helper runs a file only when it
   // has that digest, read through a handle it keeps open until the program has
   // finished. The helper does the rest: runs the installer, waits for the service
   // to come back, runs the rollback image if it does not, and writes one line
   // saying what happened, which ReportLastOutcome reads and reports the next time
   // the service starts.
   //
   // On Linux the same steps end differently. The service runs as its own user
   // under a unit that leaves /usr read-only, so it cannot install a package or
   // stop itself; it writes a REQUEST - the package, the rollback image, the unit,
   // where the outcome goes - to /run/hmailserver, and hmailserver-update.path, a
   // root path unit the package enables, starts hmailserver-update.service, which
   // runs the shell script /usr/lib/hmailserver/hmailserver-update: it checks the
   // request, verifies the package again as root, stops the unit, installs with
   // the package manager (or swaps the AppImage file), starts the unit, waits for
   // it, rolls back if it does not come back, and writes the SAME outcome line.
   // A server that is itself root under a unit reaches the script through
   // systemd-run instead; one under no unit is told to install by hand.
   //
   // A rollback reinstalls the previous binaries; it cannot take the database back.
   // When the new version had already moved the schema and then failed to start, the
   // previous version will refuse the newer schema, and the outcome says so: that is
   // the case for a backup before an update, which BackupBeforeUpdate arranges.
   class UpdateInstaller
   {
   public:
      // On the calling thread until the helper has been started; the update itself
      // happens after this returns. True when the helper was started. In a snap,
      // a container or a build without the live update (HM_LIVE_UPDATE=0, which
      // compiles none of the steps above), false at once, and error names what
      // updates this installation.
      static bool Apply(String &error);

      // At startup: the outcome the helper wrote, if any, into the log and the
      // verdict, then out of the way.
      static void ReportLastOutcome();

      // At startup, once per process, after ReportLastOutcome: the outcome of an
      // apply whose helper has not written it yet. The helper writes it only once
      // it has SEEN the service running, and this service reports running to the
      // service manager before it loads anything, so the start-up run of the
      // update task can look for the file a second before it is there - and the
      // next run is a quarter of an hour away. When the helper's log in the
      // Updates directory is newer than the last outcome reported, the helper is
      // still at work: the file is looked for every second, for the helper's own
      // wait plus two minutes, and reported when it has been seen twice. On the
      // task's thread, whose sleep a service stop interrupts.
      static void AwaitOutcomeAtStart();

      // Bin\hMailServer.Updater.exe, beside the running executable; on Linux the
      // packaged /usr/lib/hmailserver/hmailserver-update, or the copy an AppImage
      // carries at the same relative path.
      static String HelperSourcePath();
      static String OutcomePath();

      // Whether this service may apply an update itself. On Windows the helper
      // runs the installer as the service's own account, and the installer takes
      // an administrator; LocalSystem is one, and the virtual account
      // NT SERVICE\hMailServer a new installation runs as is not
      // (docs/ServiceAccount.md). So under such an account the update is checked
      // here and installed by an administrator, and why says so: download it from
      // the release page (releaseUrl) and check its Authenticode signature - never
      // the copy in the Updates folder, which the service can write. No scheduled
      // task running as SYSTEM stands in for it: a service that could make SYSTEM
      // run a file it had written would have given back what the account took
      // away. True on POSIX, where the packaged path unit runs the helper as
      // root on the service's request - except in a snap, a container or a build
      // without the live update, which what installed it updates, and why names
      // how (UpdateChecker::UpdatedElsewhere).
      static bool CanApplyHere(const String &releaseUrl, String &why);

   private:
      static bool EnsureRollbackImage_(const String &runningVersion, String &rollbackPath, String &why);

      // A helper's log in the Updates directory - apply-<version>.log, or on Linux
      // the apply.XXXXXX it writes until it exits - written within the hour and
      // after the last reported outcome (last-apply.txt.reported), if any.
      static bool HelperStillAtWork_();

      // Watches the helper Apply started, on a thread of its own, for as long as
      // the drain Apply entered is still this drain: the helper is meant to stop
      // the service, after which this process is gone and the outcome is read
      // at the next start - but a helper that finishes without stopping it (an
      // installer that failed while the service stayed up, one that did nothing)
      // or never gets that far would otherwise leave the server draining for
      // ever, since nothing else would end it. So: an outcome file seen on two
      // consecutive polls, or UPDATE_APPLY_DRAIN_MINUTES with no stop, leaves
      // the drain as the server, says why, and reports the outcome then.
      static void WatchTheHelper_(const String &drainSince);
      // Windows: starts the helper detached with the assembled command line.
      static bool Launch_(const String &commandLine, const String &workingDirectory, String &error);
      // POSIX: hands the request to hmailserver-update through the path unit, or
      // through systemd-run when nothing takes it from /run/hmailserver.
      static bool LaunchPosix_(const AnsiString &request, const String &version, const String &helperSource, String &error);
   };
}
