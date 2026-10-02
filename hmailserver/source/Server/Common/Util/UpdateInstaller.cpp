// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "UpdateInstaller.h"
#include "UpdateApplyToken.h"
#include "UpdateChecker.h"
#include "UpdateDownloader.h"
#include "SigstoreVerifier.h"
#include "ServiceAccount.h"
#include "FileUtilities.h"
#include "ProtectedFile.h"
#include "Unicode.h"
#include "Time.h"
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../Application/ErrorManager.h"
#include "../Application/BackupManager.h"
#include "AuditTrail.h"
#ifndef HM_PLATFORM_POSIX
#include "AdminUpdate.h"
#endif

#include <boost/thread/thread.hpp>
#include <boost/filesystem.hpp>
#include <atomic>
#include <ctime>

#ifdef HM_PLATFORM_POSIX
#include "ProcessLauncher.h"
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
#ifdef HM_PLATFORM_POSIX
      // Where the .deb and the .rpm install the helper, and where an AppImage
      // carries it relative to its own root.
      const char *PACKAGED_HELPER = "/usr/lib/hmailserver/hmailserver-update";
      const char *APPIMAGE_HELPER = "/usr/lib/hmailserver/hmailserver-update";
#endif

      // The apply step's own constants. A build without the live update
      // (HM_LIVE_UPDATE=0, UpdateChecker.h) compiles none of the apply step
      // below - no hand-over to a helper, no rollback image, no drain - and so
      // none of these either.
#if HM_LIVE_UPDATE
#ifdef HM_PLATFORM_POSIX
      const TCHAR *HELPER_NAME = _T("hmailserver-update");
      // How long the path unit is given to take a request before the next route
      // is tried. It reacts through inotify, within a second; fifteen is
      // generous, and a request nobody takes in that time has no taker.
      const int PATH_UNIT_WAIT_MILLISECONDS = 15000;
#else
      const TCHAR *SERVICE_NAME = _T("hMailServer");
      const TCHAR *HELPER_NAME = _T("hMailServer.Updater.exe");
#endif

      String Quote_(const String &text)
      {
         return _T("\"") + text + _T("\"");
      }

      // How long a drain the apply entered may last with the helper neither
      // stopping the service nor writing an outcome, and how often the watcher
      // looks. A constant rather than a setting, deliberately: the helper stops
      // the service within seconds on every path that works, and the number only
      // has to be long enough that a slow installer's own wait is not cut short
      // and short enough that a server is not left refusing connections for an
      // afternoon because of a helper that gave up in silence. Upgrading.md and
      // README.md name the ten minutes.
      const int UPDATE_APPLY_DRAIN_MINUTES = 10;
      const int UPDATE_APPLY_WATCH_POLL_SECONDS = 5;

      // update-backup-<version>.txt in the Updates folder: where the backup this
      // install of version took before it is - the backup destination, then the time
      // the backup finished - or no file, when it took none. hMailServer.Updater
      // reads it into the outcome of an install whose database step changed the
      // database and that did not complete (BackupNote in hMailServer.Updater.cpp),
      // which says to restore that backup to go back: the earlier version may not run
      // on the changed database, and is not put back. Text for a person and nothing
      // else; nothing reads a path from it. Named for the version, as the sweep
      // keeps a file that names a version in play (UpdateCheckTask). Windows only in
      // effect: an administrator's install, and the Windows apply, are the callers.
      void RecordBackupForHelper_(const String &version, const String &folder)
      {
         const String directory = UpdateDownloader::UpdatesDirectory();
         if (directory.IsEmpty() || version.IsEmpty())
            return;
         const String path = directory + FileUtilities::PathSeparator + _T("update-backup-") + version + _T(".txt");
         if (FileUtilities::Exists(path))
            FileUtilities::DeleteFile(path);
         if (folder.IsEmpty())
            return;
         FileUtilities::WriteToFile(path, Unicode::ToUtf8(folder + _T("\r\n") + Time::GetCurrentDateTime() + _T("\r\n")));
      }
#endif

      // AwaitOutcomeAtStart: how long past the helper's own wait for the service
      // the outcome is looked for, and how recent the helper's log must be for an
      // apply to count as still in progress (the helper gives an installer an
      // hour).
      const int UPDATE_OUTCOME_GRACE_SECONDS = 120;
      const int UPDATE_HELPER_RECENT_SECONDS = 3600;
   }

#if HM_LIVE_UPDATE
#ifdef HM_PLATFORM_POSIX
   UpdateInstaller::HelperUnitState
   UpdateInstaller::ReadHelperUnitState(const AnsiString &showOutput, int &exitStatus)
   {
      exitStatus = -1;
      std::string activeState;

      const std::string text(showOutput.c_str(), showOutput.GetLength());
      size_t start = 0;
      while (start < text.size())
      {
         size_t end = text.find('\n', start);
         if (end == std::string::npos)
            end = text.size();
         std::string line = text.substr(start, end - start);
         start = end + 1;

         while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

         const size_t equals = line.find('=');
         if (equals == std::string::npos)
            continue;

         const std::string key = line.substr(0, equals);
         const std::string value = line.substr(equals + 1);
         if (key == "ActiveState")
            activeState = value;
         else if (key == "ExecMainStatus" && !value.empty() && value.size() <= 9 &&
                  value.find_first_not_of("0123456789") == std::string::npos)
            exitStatus = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
      }

      if (activeState == "activating" || activeState == "active" || activeState == "deactivating" ||
          activeState == "reloading" || activeState == "refreshing")
         return HelperUnitAtWork;

      if (activeState == "inactive" || activeState == "failed")
         return HelperUnitEnded;

      return HelperUnitUnknown;
   }

   UpdateInstaller::HelperUnitState
   UpdateInstaller::AskHelperUnitState(const String &systemctlPath, const String &unit, const String &answerFile, int &exitStatus)
   {
      exitStatus = -1;

      // A unit name is what systemd names: anything else is not put on a
      // command line.
      const std::string name = std::string(AnsiString(unit).c_str());
      bool nameable = !name.empty() && name.size() <= 128;
      for (size_t i = 0; nameable && i < name.size(); ++i)
      {
         const char c = name[i];
         nameable = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    c == '.' || c == '-' || c == '_' || c == '@' || c == ':';
      }
      if (!nameable || access(std::string(AnsiString(systemctlPath).c_str()).c_str(), X_OK) != 0)
         return HelperUnitUnknown;

      const String directory = FileUtilities::GetFilePath(answerFile);
      ProcessLauncher launcher(systemctlPath + _T(" show --property=LoadState --property=ActiveState --property=ExecMainStatus ") + unit, directory);
      launcher.SetStandardOutputFile(answerFile);
      launcher.SetMaximumRunTime(10000);

      unsigned int exitCode = 0;
      const bool ran = launcher.Launch(exitCode) && exitCode == 0;
      AnsiString answer;
      if (ran)
         answer = AnsiString(FileUtilities::ReadCompleteTextFile(answerFile));
      FileUtilities::DeleteFile(answerFile);

      return ran ? ReadHelperUnitState(answer, exitStatus) : HelperUnitUnknown;
   }
#endif

   void
   UpdateInstaller::WatchTheHelper_(const String &drainSince, const String &helperUnit)
   {
      const ULONGLONG deadline = GetTickCount64() + (ULONGLONG) UPDATE_APPLY_DRAIN_MINUTES * 60 * 1000;
      bool outcomeSeen = false;
#ifdef HM_PLATFORM_POSIX
      const char *systemctl = access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl"
                            : access("/bin/systemctl", X_OK) == 0 ? "/bin/systemctl" : nullptr;
#else
      (void) helperUnit;
#endif

      for (;;)
      {
         Sleep(UPDATE_APPLY_WATCH_POLL_SECONDS * 1000);

         // The state first, and nothing else when it is not this drain: the stop
         // the helper issued has come (Stopping, and this process is on its way
         // out), a reinitialise ended it, or somebody left it over the route -
         // each of which has already said so.
         Application::DrainStatus drain = Application::Instance()->GetDrainStatus();
         if (!drain.draining || drain.since != drainSince)
            return;

         String outcome = OutcomePath();
         if (!outcome.IsEmpty() && FileUtilities::Exists(outcome))
         {
            // Seen on two consecutive polls, so that whoever wrote it has finished
            // writing and whoever was waiting to read it has read it.
            if (!outcomeSeen)
            {
               outcomeSeen = true;
               continue;
            }

            LOG_APPLICATION(Formatter::Format(_T("Update: the helper has finished while this service is still running, so the service was not stopped and this is still hMailServer {0}; the drain is left and the outcome is reported now rather than at the next start."),
               UpdateChecker::RunningVersion()));
            Application::Instance()->LeaveDrainAsTheServer(_T("The update helper finished without stopping the service; its outcome is reported now."));
            ReportLastOutcome();
            return;
         }

#ifdef HM_PLATFORM_POSIX
         // The helper's unit, asked of systemd. A helper that refuses the request
         // - an AppImage under the path unit without the --image drop-in, a line
         // that fails a check - exits before it stops anything and writes
         // nothing into the Updates directory, which is this service's user's
         // (hmailserver-update says why). Ended with no outcome while this
         // service still runs, it is not going to stop it: the drain is left now,
         // where until 6.3.6 the server refused connections for the ten minutes
         // below. When systemd cannot be asked, the ten minutes stand.
         if (systemctl && !helperUnit.IsEmpty())
         {
            int exitStatus = -1;
            const String answerFile = UpdateDownloader::UpdatesDirectory() + FileUtilities::PathSeparator + _T("helper-unit-state.txt");
            if (AskHelperUnitState(String(systemctl), helperUnit, answerFile, exitStatus) == HelperUnitEnded)
            {
               // An outcome written just before it ended is the better account,
               // and the next poll reports it.
               if (!outcome.IsEmpty() && FileUtilities::Exists(outcome))
                  continue;

               String how = _T("");
               if (exitStatus == 4)
                  how = _T(": it refused the request (exit status 4)");
               else if (exitStatus >= 0)
                  how = Formatter::Format(_T(" (exit status {0})"), exitStatus);

               String message = Formatter::Format(_T("The update helper {0} ended without stopping the service and wrote no outcome{1}, so the drain the apply entered has been left at once and the service accepts again. journalctl -u {2} says why; the verified package is where the download put it, in {3}."),
                  helperUnit, how, helperUnit, UpdateDownloader::UpdatesDirectory());
               ErrorManager::Instance()->ReportError(ErrorManager::High, 7151, "UpdateInstaller::WatchTheHelper_", message);
               UpdateChecker::RecordFailure(message);
               Application::Instance()->LeaveDrainAsTheServer(Formatter::Format(_T("The update helper {0} ended without stopping the service or writing an outcome."), helperUnit));
               return;
            }
         }
#endif

         if (GetTickCount64() >= deadline)
         {
            String message = Formatter::Format(_T("The update helper neither stopped the service nor wrote an outcome within {0} minutes, so the drain the apply entered has been left and the service accepts again. The verified installer is where the download put it; the helper's log in {1} says how far it got."),
               UPDATE_APPLY_DRAIN_MINUTES, UpdateDownloader::UpdatesDirectory());
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7151, "UpdateInstaller::WatchTheHelper_", message);
            UpdateChecker::RecordFailure(message);
            Application::Instance()->LeaveDrainAsTheServer(Formatter::Format(_T("The update helper neither stopped the service nor wrote an outcome within {0} minutes."), UPDATE_APPLY_DRAIN_MINUTES));
            return;
         }
      }
   }
#endif

   String
   UpdateInstaller::HelperSourcePath()
   {
#ifdef HM_PLATFORM_POSIX
      // The helper the package installed, or the one the AppImage carries. It
      // is a shell script - the whole of what root does to apply an update, in a
      // form that can be read - and hmailserver-update.service runs the packaged
      // copy by its own path; this function's answer is what Apply checks is
      // there, and what the systemd-run route copies out of an AppImage.
      switch (UpdateChecker::DetectInstallKind())
      {
      case UpdateChecker::InstallDeb:
      case UpdateChecker::InstallRpm:
         return String(PACKAGED_HELPER);
      case UpdateChecker::InstallAppImage:
      {
         // APPDIR is the image's mount point, set by the runtime with APPIMAGE.
         const char *appdir = getenv("APPDIR");
         if (!appdir || !*appdir)
            return _T("");
         return String(appdir) + String(APPIMAGE_HELPER);
      }
      default:
         return _T("");
      }
#else
      wchar_t module[MAX_PATH];
      DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
      if (length == 0 || length >= MAX_PATH)
         return _T("");
      String path = module;
      int slash = path.ReverseFind('\\');
      if (slash < 0)
         return _T("");
      return path.Left(slash + 1) + HELPER_NAME;
#endif
   }

   bool
   UpdateInstaller::CanApplyHere(const String &releaseUrl, String &why)
   {
      why.Empty();

#ifdef HM_PLATFORM_POSIX
      (void) releaseUrl;
      // A snap, a container or a build without the live update: what installed
      // it updates it, and why names how (UpdateChecker::UpdatedElsewhere).
      String command;
      if (UpdateChecker::UpdatedElsewhere(UpdateChecker::DetectInstallKind(), UpdateChecker::Current().available_version, command, why))
         return false;
      return true;
#else
      if (ServiceAccount::ProcessIsAdministrator())
         return true;

      // Installed by an administrator instead, with Install update in the
      // Control Panel on this server, which copies the installer out of the
      // folder the service writes and checks it itself - elevated, after
      // Windows' own prompt - before it runs it (Util/AdminUpdate.h). Never by
      // anything this service could start: a service that could make an
      // administrator or SYSTEM run a file has given back what the account took
      // away (docs/ServiceAccount.md).
      (void) releaseUrl;
      why = InstallAdvice();
      return false;
#endif
   }

   String
   UpdateInstaller::OutcomePath()
   {
      String directory = UpdateDownloader::UpdatesDirectory();
      return directory.IsEmpty() ? String() : directory + FileUtilities::PathSeparator + _T("last-apply.txt");
   }

   bool
   UpdateInstaller::HelperStillAtWork_()
   {
      String directory = UpdateDownloader::UpdatesDirectory();
      String outcome = OutcomePath();
      if (directory.IsEmpty() || outcome.IsEmpty())
         return false;

      const std::time_t now = std::time(nullptr);

      // The last outcome reported, whose file keeps the time the helper wrote it:
      // a log no newer than that is the log of the apply already reported.
      std::time_t reported = 0;
      boost::system::error_code reportedError;
      boost::filesystem::path reportedPath(std::wstring(outcome + _T(".reported")));
      if (boost::filesystem::exists(reportedPath, reportedError))
      {
         std::time_t written = boost::filesystem::last_write_time(reportedPath, reportedError);
         if (!reportedError)
            reported = written;
      }

      boost::system::error_code ec;
      for (boost::filesystem::directory_iterator entry(std::wstring(directory), ec); !ec && entry != boost::filesystem::directory_iterator(); entry.increment(ec))
      {
         // apply-<version>.log, the Windows helper's log from its first line; on
         // Linux hmailserver-update writes apply.XXXXXX (mktemp) and moves it to
         // apply-<version>.log only as it exits, after the outcome.
         const std::wstring name = entry->path().filename().wstring();
         const bool windowsLog = name.size() > 10 && name.compare(0, 6, L"apply-") == 0 && name.compare(name.size() - 4, 4, L".log") == 0;
         const bool posixLog = name.size() == 12 && name.compare(0, 6, L"apply.") == 0;
         if (!windowsLog && !posixLog)
            continue;

         boost::system::error_code timeError;
         std::time_t written = boost::filesystem::last_write_time(entry->path(), timeError);
         if (timeError)
            continue;
         if (written > reported && now - written < UPDATE_HELPER_RECENT_SECONDS)
            return true;
      }

      return false;
   }

   void
   UpdateInstaller::AwaitOutcomeAtStart()
   {
      // Once per process: the start-up run and the first quarter-hour run are the
      // same task, and only a start can be followed by a helper's late outcome.
      static std::atomic<bool> asked(false);
      if (asked.exchange(true))
         return;

      // An administrator's install whose Updater has not written its outcome
      // yet: it writes it once it has seen this service running, as the
      // unattended helper does.
      if (AdministratorInstallPending_())
      {
         const int seconds = IniFileSettings::Instance()->GetUpdateServiceWaitSeconds() + UPDATE_OUTCOME_GRACE_SECONDS;
         for (int waited = 0; waited < seconds && AdministratorInstallPending_(); waited++)
            boost::this_thread::sleep_for(boost::chrono::seconds(1));
         // Once more, for the file written after the mark was removed.
         boost::this_thread::sleep_for(boost::chrono::seconds(1));
         ReportLastOutcome();
         return;
      }

      String outcome = OutcomePath();
      if (outcome.IsEmpty() || FileUtilities::Exists(outcome) || !HelperStillAtWork_())
         return;

      const int seconds = IniFileSettings::Instance()->GetUpdateServiceWaitSeconds() + UPDATE_OUTCOME_GRACE_SECONDS;
      LOG_DEBUG(Formatter::Format(_T("Update: the helper's log is newer than the last outcome reported and there is no outcome yet, so the helper is still at work; its outcome is looked for over the next {0} seconds."), seconds));

      // Seen on two consecutive looks, as WatchTheHelper_ does: the writer has
      // finished writing.
      bool seen = false;
      for (int waited = 0; waited < seconds; waited++)
      {
         boost::this_thread::sleep_for(boost::chrono::seconds(1));

         if (!FileUtilities::Exists(outcome))
         {
            seen = false;
            continue;
         }

         if (!seen)
         {
            seen = true;
            continue;
         }

         ReportLastOutcome();
         return;
      }
   }

   // From here to Apply, the apply step itself: the rollback image, the hand-over
   // to the helper and the checks of what is handed over. Not compiled in a build
   // without the live update, whose Apply only says what updates it.
#if HM_LIVE_UPDATE
   bool
   UpdateInstaller::EnsureRollbackImage_(const String &runningVersion, String &rollbackPath, String &why)
   {
      rollbackPath.Empty();

      // Made safe and held by Apply (UpdateDownloader::PrepareUpdatesDirectory),
      // which is the only caller: never made here with inherited permissions.
      String directory = UpdateDownloader::UpdatesDirectory() + FileUtilities::PathSeparator + _T("rollback");
      if (!FileUtilities::DirectoryExists(directory))
      {
         why = Formatter::Format(_T("{0} is not there."), directory);
         return false;
      }

      // The running version's own artefact - the installer, or the package this
      // installation came from - by the name the release flow gave it.
      String name = UpdateChecker::PlatformInstallerName(runningVersion);
      if (name.IsEmpty())
      {
         why = _T("there is no release artefact for this kind of installation");
         return false;
      }
      String path = directory + FileUtilities::PathSeparator + name;
      String bundlePath = path + _T(".cosign.bundle");

      SigstoreTrust trust;
      if (!SigstoreVerifier::ConfiguredTrust(trust, why))
         return false;

      // Already fetched by an earlier apply, and still the file it was.
      if (FileUtilities::Exists(path) && FileUtilities::Exists(bundlePath))
      {
         SigstoreVerdict verdict;
         AnsiString bundleJson;
         SigstoreVerifier::ReadBundle(bundlePath, bundleJson);
         String versionWhy;
         if (SigstoreVerifier::VerifyFile(path, bundleJson, trust, verdict, runningVersion) &&
             UpdateDownloader::InstallerVersionIs(path, runningVersion, versionWhy))
         {
            rollbackPath = path;
            return true;
         }
         FileUtilities::DeleteFile(path);
         FileUtilities::DeleteFile(bundlePath);
      }

      UpdateChecker::ReleaseAssets assets;
      if (!UpdateChecker::FetchReleaseByTag(runningVersion, assets, why))
         return false;
      if (assets.installer_url.IsEmpty() || assets.bundle_url.IsEmpty())
      {
         why = Formatter::Format(_T("release {0} carries no {1} with a Sigstore bundle."), runningVersion, UpdateChecker::MissingArtefactDescription(runningVersion));
         return false;
      }

      SigstoreVerdict verdict;
      if (!UpdateDownloader::FetchVerified(assets.installer_url, assets.bundle_url, path, assets.installer_size, assets.installer_digest,
                                           runningVersion, trust, verdict, why))
         return false;

      rollbackPath = path;
      return true;
   }

#ifdef HM_PLATFORM_POSIX
   namespace
   {
      // The server's own unit, from the cgroup it runs in: the last component of
      // its path in /proc/self/cgroup ("0::/system.slice/hmailserver.service").
      // systemd does not put the unit's name in the environment, and the cgroup
      // is where it is written down.
      std::string OwnUnitName_()
      {
         std::ifstream cgroup("/proc/self/cgroup");
         std::string line;
         while (std::getline(cgroup, line))
         {
            size_t slash = line.rfind('/');
            if (slash == std::string::npos)
               continue;
            std::string leaf = line.substr(slash + 1);
            if (leaf.size() > 8 && leaf.compare(leaf.size() - 8, 8, ".service") == 0)
               return leaf;
         }
         return "hmailserver.service";
      }

      // The metrics listener's readiness probe, for the helper to wait on, when
      // the listener is bound where the helper can reach it.
      std::string ReadyUrl_()
      {
         IniFileSettings *settings = IniFileSettings::Instance();
         int port = settings->GetMetricsServerPort();
         if (port <= 0)
            return "";
         String bind = settings->GetMetricsServerBindAddress();
         std::string host;
         if (bind.IsEmpty() || bind == _T("0.0.0.0") || bind == _T("127.0.0.1"))
            host = "127.0.0.1";
         else if (bind == _T("::") || bind == _T("::1"))
            host = "[::1]";
         else
            return "";
         // Over TLS the probe would need the certificate; the helper's curl
         // speaks plain HTTP to loopback and nothing else, so a TLS listener
         // is left to the unit's own state.
         if (!settings->GetMetricsServerCertificateFile().IsEmpty())
            return "";
         return "http://" + host + ":" + std::to_string(port) + "/readyz";
      }

      bool RequestTaken_(const String &requestPath)
      {
         for (int waited = 0; waited < PATH_UNIT_WAIT_MILLISECONDS; waited += 100)
         {
            if (!FileUtilities::Exists(requestPath))
               return true;
            Sleep(100);
         }
         return !FileUtilities::Exists(requestPath);
      }
   }

   // The POSIX apply: the request written for hmailserver-update, and the two
   // ways of getting it to a process that runs as root outside this unit.
   //
   // The packaged route first. Under the packaged unit RUNTIME_DIRECTORY names
   // /run/hmailserver, which this process may write and hmailserver-update.path
   // watches as root; the request goes there and the path unit starts
   // hmailserver-update.service, whose first act is to remove the file. So a
   // request that is gone within seconds has been taken, and one that is still
   // there has no taker - the path unit is not enabled, or this is a unit of
   // somebody's own.
   //
   // Then systemd-run, which starts the helper as a transient unit of its own,
   // outside this one's cgroup, so that stopping this unit does not kill it.
   // It needs the privilege to start units, which the packaged service does not
   // have and is not meant to have; a server that is itself root under a unit -
   // an AppImage run as a service is the case - has it. The helper is copied out
   // of the image first, as the Windows helper is copied out of Bin: the image's
   // mount goes away with the process that mounted it.
   // A unit named for the version, collected when it is done or has failed, so
   // that a second attempt at the same version is not refused for the first
   // one's remains. The unit to stop and the image to swap are the helper's
   // command line, never the request's alone: from 6.3.6 the helper takes
   // neither from the request, and refuses an AppImage request whose command
   // line names no image. On this route the server is root (systemd-run would
   // not have been allowed otherwise), so what it knows of itself is root's
   // word. Each is passed only when it can be quoted whole; the helper then
   // refuses the request, which names it, rather than act on something else.
   String
   UpdateInstaller::SystemdRunCommand(const String &systemdRun, const String &version, const String &helper,
      const String &requestFile, const String &directory, const String &unit, const String &image)
   {
      String command = systemdRun + _T(" --unit=hmailserver-update-") + version + _T(" --collect --quiet -- ")
         + Quote_(helper) + _T(" apply --request ") + Quote_(requestFile) + _T(" --updates-dir ") + Quote_(directory);
      if (!unit.IsEmpty() && unit.Find(_T("\"")) < 0)
         command += _T(" --unit ") + Quote_(unit);
      if (!image.IsEmpty() && image.Find(_T("\"")) < 0)
         command += _T(" --image ") + Quote_(image);
      return command;
   }

   bool
   UpdateInstaller::LaunchPosix_(const AnsiString &request, const String &version, const String &helperSource,
      const String &unit, const String &image, String &helperUnit, String &error)
   {
      helperUnit.Empty();
      String directory = UpdateDownloader::UpdatesDirectory();

      const char *runtimeVariable = getenv("RUNTIME_DIRECTORY");
      std::string runtimeDirectory = runtimeVariable ? runtimeVariable : "";
      // Several RuntimeDirectory= lines make a colon-separated list; ours is one.
      size_t colon = runtimeDirectory.find(':');
      if (colon != std::string::npos)
         runtimeDirectory.resize(colon);

      String untaken;
      if (!runtimeDirectory.empty())
      {
         String requestPath = String(runtimeDirectory.c_str()) + _T("/apply-request");
         if (!FileUtilities::WriteToFileAtomically(requestPath, request))
         {
            untaken = Formatter::Format(_T("the request could not be written to {0}"), requestPath);
         }
         else
         {
            LOG_APPLICATION(Formatter::Format(_T("Update: the request to apply hMailServer {0} is at {1}; waiting for hmailserver-update.path to take it."), version, requestPath));
            if (RequestTaken_(requestPath))
            {
               LOG_APPLICATION(_T("Update: hmailserver-update.service has taken the request. It stops this unit, installs, starts it and writes the outcome."));
               helperUnit = _T("hmailserver-update.service");
               return true;
            }
            FileUtilities::DeleteFile(requestPath);
            untaken = Formatter::Format(_T("nothing took the request at {0} within {1} seconds, so hmailserver-update.path is not active (systemctl enable --now hmailserver-update.path)"),
               requestPath, PATH_UNIT_WAIT_MILLISECONDS / 1000);
         }
      }
      else
      {
         untaken = _T("RUNTIME_DIRECTORY is not set, so this server is not running under a unit with RuntimeDirectory=hmailserver and has nowhere the packaged path unit watches to put the request");
      }

      // The second route: systemd-run, which starts the helper as a transient
      // unit of its own. It needs the right to start units, which the packaged
      // service does not have (PolicyKit refuses it, and the error below says
      // so); a server that is itself root under a unit - an AppImage run that
      // way - has it. INVOCATION_ID says only that a unit is running us, not
      // who we are; systemd-run's own answer settles the rest.
      const bool underSystemd = getenv("INVOCATION_ID") != nullptr;
      const char *systemdRun = access("/usr/bin/systemd-run", X_OK) == 0 ? "/usr/bin/systemd-run"
                             : access("/bin/systemd-run", X_OK) == 0 ? "/bin/systemd-run" : nullptr;
      if (underSystemd && systemdRun)
      {
         // The version names the transient unit, so it has to be a unit name's
         // kind of text. It comes from the release feed, not from anything a
         // client sent, and a release is tagged with letters, digits, dots and
         // dashes; anything else is refused rather than put on a command line.
         std::string plain = std::string(AnsiString(version).c_str());
         bool nameable = !plain.empty() && plain.size() <= 64;
         for (size_t i = 0; nameable && i < plain.size(); ++i)
         {
            char c = plain[i];
            nameable = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_';
         }
         if (!nameable)
         {
            error = Formatter::Format(_T("The version {0} is not a name a unit can carry, so the helper was not started."), version);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6891, "UpdateInstaller::LaunchPosix_", error);
            return false;
         }

         String requestFile = directory + _T("/apply-") + version + _T(".request");
         if (!FileUtilities::WriteToFileAtomically(requestFile, request))
         {
            error = Formatter::Format(_T("The apply request could not be written to {0}."), requestFile);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6891, "UpdateInstaller::LaunchPosix_", error);
            return false;
         }

         String helper = directory + _T("/") + HELPER_NAME;
         if (FileUtilities::Exists(helper))
            FileUtilities::DeleteFile(helper);
         if (!FileUtilities::Copy(helperSource, helper, false))
         {
            error = Formatter::Format(_T("{0} could not be copied to {1}."), helperSource, helper);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6891, "UpdateInstaller::LaunchPosix_", error);
            return false;
         }
         chmod(std::string(AnsiString(helper).c_str()).c_str(), 0755);

         // The unit this server runs under, and for an AppImage the image it runs
         // from, are passed by Apply and go on the helper's command line
         // (SystemdRunCommand), never taken by the helper from the request alone.
         String command = SystemdRunCommand(String(systemdRun), version, helper, requestFile, directory,
            unit, image);
         LOG_APPLICATION(Formatter::Format(_T("Update: {0}; starting the helper with systemd-run: {1}"), untaken, command));

         ProcessLauncher launcher(command, directory);
         launcher.SetMaximumRunTime(30000);
         unsigned int exitCode = 0;
         if (launcher.Launch(exitCode) && exitCode == 0)
         {
            LOG_APPLICATION(_T("Update: systemd-run started the helper as a unit of its own. It stops this unit, applies, starts it and writes the outcome."));
            helperUnit = _T("hmailserver-update-") + version + _T(".service");
            return true;
         }

         FileUtilities::DeleteFile(requestFile);
         error = Formatter::Format(_T("This server cannot hand the update to a process that could apply it: {0}, and systemd-run exited with {1} - it needs the privilege to start a unit, which the packaged service deliberately lacks. Install the new version the way this one was installed."),
            untaken, (int) exitCode);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6892, "UpdateInstaller::LaunchPosix_", error);
         return false;
      }

      error = Formatter::Format(_T("This server cannot hand the update to a process that could apply it: {0}{1}. The verified file is waiting where the download put it; install it the way this version was installed."),
         untaken, underSystemd ? String(_T(", and systemd-run is not on this machine")) : String(_T(", and this server is not running under systemd, so there is no unit for a helper to stop and start")));
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6890, "UpdateInstaller::LaunchPosix_", error);
      return false;
   }
#else
   bool
   UpdateInstaller::Launch_(const String &commandLine, const String &workingDirectory, String &error)
   {
      std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
      mutableCommand.push_back(L'\0');

      STARTUPINFOW startup;
      memset(&startup, 0, sizeof(startup));
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process;
      memset(&process, 0, sizeof(process));

      // Detached from this process in every way Windows offers: its own group, no
      // console, and out of any job the service runs in, so that the service
      // stopping - which is what the installer does first - does not take the
      // helper with it. Breaking away needs the job to allow it; when it does not,
      // the helper is started inside it and the service's own exit is the only risk.
      DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
      BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, flags, nullptr, workingDirectory.c_str(), &startup, &process);
      if (!started && GetLastError() == ERROR_ACCESS_DENIED)
      {
         flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
         started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, flags, nullptr, workingDirectory.c_str(), &startup, &process);
      }
      if (!started)
      {
         error = Formatter::Format(_T("The update helper could not be started: Windows error {0}."), (int) GetLastError());
         return false;
      }

      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
   }
#endif

   namespace
   {
      enum CheckResult_
      {
         CheckPassed_,
         CheckBundleRefused_,
         CheckVersionRefused_,
         CheckUnreadable_
      };

      // A file the apply hands on - the installer or the rollback image - against
      // its bundle (<path>.cosign.bundle) and the version its resource names. With
      // sha256 given, all of it with the file held open (FILE_SHARE_READ and
      // nothing more, so nothing can write, rename or delete it, nor rename a
      // folder above it, until the checks are done), and its SHA-256 read under
      // the same hold: the digest the helper requires is then of the bytes that
      // passed. The signature, the version resource and the digest are three reads
      // of the file; held, they are reads of the same bytes.
      CheckResult_ Check_(const String &path, const String &version, const SigstoreTrust &trust, std::string *sha256, String &why)
      {
#ifdef _WIN32
         HANDLE held = INVALID_HANDLE_VALUE;
         if (sha256 != nullptr)
         {
            held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (held == INVALID_HANDLE_VALUE)
            {
               why = Formatter::Format(_T("{0} could not be held open to be checked (Windows error {1})."), path, (int) GetLastError());
               return CheckUnreadable_;
            }
         }
#endif

         CheckResult_ result = CheckPassed_;
         String bundlePath = path + _T(".cosign.bundle");
         AnsiString bundleJson;
         if (FileUtilities::Exists(bundlePath))
            SigstoreVerifier::ReadBundle(bundlePath, bundleJson);
         SigstoreVerdict verdict;
         if (!SigstoreVerifier::VerifyFile(path, bundleJson, trust, verdict, version))
         {
            why = verdict.error;
            result = CheckBundleRefused_;
         }
         else if (!UpdateDownloader::InstallerVersionIs(path, version, why))
            result = CheckVersionRefused_;
         else if (sha256 != nullptr)
         {
            std::vector<unsigned char> digest;
            if (SigstoreVerifier::Sha256File(path, digest, why))
               *sha256 = SigstoreVerifier::Hex(digest);
            else
               result = CheckUnreadable_;
         }

#ifdef _WIN32
         if (held != INVALID_HANDLE_VALUE)
            CloseHandle(held);
#endif
         return result;
      }

      // The waiting installer, checked as Check_ does; false with error, the
      // failure recorded and - where it is not the release it was downloaded as -
      // the file and its bundle deleted.
      bool CheckWaitingInstaller_(const UpdateChecker::Snapshot &snapshot, const SigstoreTrust &trust, std::string *sha256, String &error)
      {
         String why;
         const CheckResult_ result = Check_(snapshot.installer_path, snapshot.available_version, trust, sha256, why);
         if (result == CheckPassed_)
            return true;

         if (result == CheckUnreadable_)
            error = Formatter::Format(_T("The installer could not be checked: {0}"), why);
         else
         {
            String bundlePath = snapshot.installer_path + _T(".cosign.bundle");
            FileUtilities::DeleteFile(snapshot.installer_path);
            if (FileUtilities::Exists(bundlePath))
               FileUtilities::DeleteFile(bundlePath);
            if (result == CheckBundleRefused_)
               error = Formatter::Format(_T("The installer no longer verifies against its bundle and has been deleted: {0}"), why);
            else
               error = Formatter::Format(_T("The installer is not the release it was downloaded as and has been deleted: {0}"), why);
         }
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
         return false;
      }

#ifdef _WIN32
      // The path Windows resolved an open handle to, with the \\?\ prefix taken
      // off an ordinary path; false when it cannot say, and then nothing is
      // started - the unresolved path is the one that might lead elsewhere. A
      // program started by it is the file the handle holds: no link along the
      // way is followed again.
      bool FinalPathOf_(HANDLE handle, String &resolved)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetFinalPathNameByHandleW(handle, &buffer[0], (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (length == 0 || length >= buffer.size())
            return false;

         std::wstring path(&buffer[0], length);
         if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
            path = L"\\\\" + path.substr(8);
         else if (path.compare(0, 4, L"\\\\?\\") == 0)
            path = path.substr(4);
         resolved = String(path.c_str());
         return true;
      }
#endif
   }
#endif

   bool
   UpdateInstaller::Apply(String &error, const String &backupFolder)
   {
      error.Empty();
      // Only the Windows helper reads it (RecordBackupForHelper_).
      (void) backupFolder;

      UpdateChecker::Snapshot snapshot = UpdateChecker::Current();

      // A snap, a container or a build without the live update is updated by
      // what installed it, and says so before anything else: there is never a
      // downloaded file here to speak of (UpdateDownloader refuses to fetch one),
      // and "download one first" would send the administrator the wrong way.
      {
         String command;
         if (UpdateChecker::UpdatedElsewhere(UpdateChecker::DetectInstallKind(), snapshot.available_version, command, error))
         {
            LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
            return false;
         }
      }

#if !HM_LIVE_UPDATE
      // Not reached: every kind a build without the live update detects is one
      // UpdatedElsewhere answers for. Said all the same, rather than compiled
      // into a function that could return nothing.
      error = _T("This build of hMailServer was built without the live update, so it installs no release file.");
      return false;
#else
      if (snapshot.state != UpdateChecker::StateDownloaded || snapshot.installer_path.IsEmpty() || !FileUtilities::Exists(snapshot.installer_path))
      {
         error = _T("No verified installer is waiting; download one first.");
         return false;
      }

#ifdef HM_PLATFORM_POSIX
      const UpdateChecker::InstallKind kind = UpdateChecker::DetectInstallKind();
      if (kind == UpdateChecker::InstallNone)
      {
         error = _T("This server was not installed from a .deb, an .rpm or an AppImage, so no release artefact could replace it; install the new version the way this one was installed.");
         return false;
      }
#else
      // Before the verification, the rollback image and the drain: none of them
      // is worth doing for an installer this service cannot run.
      String refusal;
      if (!CanApplyHere(snapshot.release_url, refusal))
      {
         error = refusal;
         return false;
      }
#endif

      String helperSource = HelperSourcePath();
      if (helperSource.IsEmpty() || !FileUtilities::Exists(helperSource))
      {
         error = Formatter::Format(_T("The update helper {0} is not beside the server; this installation cannot apply updates."), helperSource);
         return false;
      }

      // The release the waiting installer was downloaded as, which every check
      // below holds it to. Empty or not newer, it is held to nothing - a check that
      // answered while a download ran could once leave it so - and nothing is
      // installed as it.
      if (snapshot.available_version.IsEmpty() || UpdateChecker::CompareVersions(snapshot.available_version, UpdateChecker::RunningVersion()) <= 0)
      {
         error = Formatter::Format(_T("The waiting installer is not for a release newer than this server's {0}; check for an update and download it again."),
            UpdateChecker::RunningVersion());
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
         return false;
      }

      String directory = UpdateDownloader::UpdatesDirectory();
      String running = UpdateChecker::RunningVersion();

      // Nobody but SYSTEM, the Administrators and the service may change what is
      // in the Updates folder - the installer, the rollback image, the helper - or
      // a file checked below could be another by the time it runs. Both folders
      // are held from here until this returns, after the helper has been started:
      // nothing checked below can be moved from under the check by renaming a
      // folder it is in, or one above that.
      UpdateDownloader::UpdatesFolders folders;
      if (!UpdateDownloader::PrepareUpdatesDirectory(folders, error))
      {
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
         return false;
      }
      // A folder others could write to is emptied when it is made safe, and the
      // waiting installer with it: it was there while anybody could replace it.
      if (!FileUtilities::Exists(snapshot.installer_path))
      {
         error = _T("The Updates folder was made safe, which removed the installer that was waiting there; download it again.");
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
         return false;
      }

      SigstoreTrust trust;
      String why;
      if (!SigstoreVerifier::ConfiguredTrust(trust, why))
      {
         error = why;
         return false;
      }

      // A first look before the rollback image is fetched, which may take
      // minutes: an installer that no longer verifies is refused without that
      // download. It is checked again, last, below.
      if (!CheckWaitingInstaller_(snapshot, trust, nullptr, error))
         return false;

      String rollbackPath;
      String rollbackWhy;
      std::string rollbackSha256;
      // The image as EnsureRollbackImage_ verified it, then held and checked once
      // more for the digest the helper requires of it before it runs it.
      bool rollbackReady = EnsureRollbackImage_(running, rollbackPath, rollbackWhy);
      if (rollbackReady && Check_(rollbackPath, running, trust, &rollbackSha256, rollbackWhy) != CheckPassed_)
      {
         rollbackReady = false;
         rollbackPath.Empty();
      }
      // Braces on both branches: LOG_APPLICATION is itself an if statement.
      if (rollbackReady)
      {
         LOG_APPLICATION(Formatter::Format(_T("Update: the rollback image for hMailServer {0} is {1}, verified."), running, rollbackPath));
      }
      else
      {
         rollbackPath.Empty();
         LOG_APPLICATION(Formatter::Format(_T("Update: no rollback image for hMailServer {0} could be had ({1}); if {2} does not start, it will need a manual reinstall."),
            running, rollbackWhy, snapshot.available_version));
      }

      // Verified once more, last, because the file has been sitting in a directory
      // since it was, and the rollback image above may have taken minutes. Held
      // while it is checked, and its SHA-256 read under the same hold: the helper
      // runs only a file with that digest, which it reads through a handle of its
      // own that it keeps until the installer has finished.
      std::string installerSha256;
      if (!CheckWaitingInstaller_(snapshot, trust, &installerSha256, error))
         return false;

      String outcome = OutcomePath();
      if (FileUtilities::Exists(outcome))
         FileUtilities::DeleteFile(outcome);

      // The drain, before the helper. The listeners close and the delivery
      // queue pauses now, so that the service stop the helper issues in a
      // moment finds the sessions and deliveries already finishing - or
      // finished - and spends its wait (ShutdownDrainSeconds, plus thirty on
      // the Windows installer's side) on that tail rather than on connections
      // accepted meanwhile. A server that is already draining is somebody
      // else's maintenance, which this apply neither takes over nor ends: it is
      // refused with the reason, and the operator leaves the drain or applies
      // once it is over. A helper that could not be started leaves the drain
      // again, so a refused apply does not leave the doors closed.
      String drainRefusal = Application::Instance()->EnterDrain();
      if (!drainRefusal.IsEmpty())
      {
         error = Formatter::Format(_T("The update was not applied: the server is {0}. Leave the drain (DELETE /api/v1/server/drain), or wait for it to end, and apply again."), drainRefusal);
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply refused: {0}"), error));
         return false;
      }

      // When this drain began: the watcher started below leaves the drain only
      // while it is still this one, and not a drain an operator entered later.
      const String drainSince = Application::Instance()->GetDrainStatus().since;

      struct LeaveDrainOnFailure
      {
         LeaveDrainOnFailure() : keep(false) {}
         // Leaves the drain when it goes: a copy would leave it twice.
         LeaveDrainOnFailure(const LeaveDrainOnFailure &) = delete;
         LeaveDrainOnFailure &operator=(const LeaveDrainOnFailure &) = delete;
         ~LeaveDrainOnFailure()
         {
            if (keep)
               return;
            std::vector<String> notReopened;
            Application::Instance()->LeaveDrain(notReopened);
         }
         bool keep;
      } drain;

#ifdef HM_PLATFORM_POSIX
      // No token here. On Windows the token is what the silent installer's
      // DBUpdater presents in place of the administrator password; the .deb and
      // the .rpm run the schema upgrade in their own post-install step, as the
      // service user, with `hmailserver --upgrade-database`, which needs no
      // credential - and an AppImage swap moves no schema at all.
      //
      // The request, one key=value per line, in the form hmailserver-update's
      // header documents. Paths only, no quoting: the helper checks each one
      // and refuses anything outside the Updates directory.
      AnsiString request;
      request += "kind=" + AnsiString(UpdateChecker::InstallKindName(kind)) + "\n";
      request += "version=" + AnsiString(snapshot.available_version) + "\n";
      request += "package=" + AnsiString(snapshot.installer_path) + "\n";
      if (!rollbackPath.IsEmpty())
      {
         request += "rollback=" + AnsiString(rollbackPath) + "\n";
         request += "rollback_version=" + AnsiString(running) + "\n";
      }
      // The unit this server runs under and, for an AppImage, the image it runs
      // from, read once here: written into the request for the helper to check
      // against, and passed to LaunchPosix_ for the systemd-run route, which puts
      // them on the helper's own (root's) command line. The helper from 6.3.6
      // takes neither from the request alone.
      String ownUnit = String(OwnUnitName_().c_str());
      String appImage;
      if (kind == UpdateChecker::InstallAppImage)
      {
         const char *image = getenv("APPIMAGE");
         if (image)
            appImage = String(image);
      }
      request += "unit=" + AnsiString(ownUnit) + "\n";
      request += "outcome=" + AnsiString(outcome) + "\n";
      request += "log=" + AnsiString(directory) + "/apply-" + AnsiString(snapshot.available_version) + ".log\n";
      request += "wait=" + AnsiString(Formatter::Format(_T("{0}"), IniFileSettings::Instance()->GetUpdateServiceWaitSeconds())) + "\n";
      // No key says that the server has drained: the helper's stop drains as it
      // always does, with the doors already closed by now, and the server's own
      // log says so. A key the helper does not know would have been a refused
      // request on an AppImage host, whose script the swap does not refresh.
      std::string readyUrl = ReadyUrl_();
      if (!readyUrl.empty())
         request += "ready_url=" + AnsiString(readyUrl.c_str()) + "\n";
      if (kind == UpdateChecker::InstallAppImage)
         request += "image=" + AnsiString(appImage) + "\n";

      String helperUnit;
      if (!LaunchPosix_(request, snapshot.available_version, helperSource, ownUnit, appImage, helperUnit, why))
      {
         UpdateChecker::RecordFailure(why);
         LOG_APPLICATION(Formatter::Format(_T("Update apply failed: {0}"), why));
         error = why;
         return false;
      }

      drain.keep = true;
      UpdateChecker::RecordInstalling();
      LOG_APPLICATION(Formatter::Format(_T("Update: hMailServer {0} is being applied by hmailserver-update; the server has drained, the service will stop and start, and the outcome is reported when it does."),
         snapshot.available_version));
      boost::thread(&UpdateInstaller::WatchTheHelper_, drainSince, helperUnit).detach();
      return true;
#else
      AnsiString token;
      if (!UpdateApplyToken::Issue(token, why))
      {
         error = why;
         return false;
      }

      String helper = directory + FileUtilities::PathSeparator + HELPER_NAME;
      if (FileUtilities::Exists(helper))
         FileUtilities::DeleteFile(helper);
      if (!FileUtilities::Copy(helperSource, helper, false))
      {
         UpdateApplyToken::Revoke();
         error = Formatter::Format(_T("{0} could not be copied to {1}."), helperSource, helper);
         return false;
      }

      // The helper is what holds the installer to its digest, so the copy is
      // held too, from before it is compared with Bin's own until it has been
      // started - by the path Windows resolved the held handle to, so that no
      // link or renamed folder along the way makes it another program.
      HANDLE heldHelper = CreateFileW(helper.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      std::vector<unsigned char> sourceDigest;
      std::vector<unsigned char> copyDigest;
      String digestError;
      String launchPath;
      if (heldHelper == INVALID_HANDLE_VALUE ||
          !SigstoreVerifier::Sha256File(helperSource, sourceDigest, digestError) ||
          !SigstoreVerifier::Sha256File(helper, copyDigest, digestError) ||
          sourceDigest != copyDigest ||
          !FinalPathOf_(heldHelper, launchPath))
      {
         if (heldHelper != INVALID_HANDLE_VALUE)
            CloseHandle(heldHelper);
         UpdateApplyToken::Revoke();
         error = Formatter::Format(_T("The update helper at {0} could not be held and shown to be {1} byte for byte, so it was not started."), helper, helperSource);
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply failed: {0}"), error));
         return false;
      }

      // Where the backup taken for this apply is, if one was, for the outcome of an
      // install that upgrades the database and does not complete; an earlier
      // apply's note of the same version goes either way.
      RecordBackupForHelper_(snapshot.available_version, backupFolder);

      String command = Quote_(launchPath);
      command += _T(" --installer ") + Quote_(snapshot.installer_path);
      command += _T(" --installer-sha256 ") + String(installerSha256.c_str());
      command += _T(" --version ") + snapshot.available_version;
      if (!rollbackPath.IsEmpty())
      {
         command += _T(" --rollback ") + Quote_(rollbackPath);
         command += _T(" --rollback-sha256 ") + String(rollbackSha256.c_str());
         command += _T(" --rollback-version ") + running;
      }
      command += _T(" --service ") + String(SERVICE_NAME);
      // The installation, the directory above Bin: the helper ends whatever is
      // running from under it before the installer replaces those files.
      int binSlash = helperSource.ReverseFind('\\');
      if (binSlash > 0)
      {
         String binDirectory = helperSource.Left(binSlash);
         int rootSlash = binDirectory.ReverseFind('\\');
         if (rootSlash > 0)
            command += _T(" --app ") + Quote_(binDirectory.Left(rootSlash));
      }
      command += _T(" --token ") + String(token);
      command += _T(" --outcome ") + Quote_(outcome);
      command += _T(" --log ") + Quote_(directory + FileUtilities::PathSeparator + _T("apply-") + snapshot.available_version + _T(".log"));
      command += Formatter::Format(_T(" --wait {0}"), IniFileSettings::Instance()->GetUpdateServiceWaitSeconds());

      const bool launched = Launch_(command, directory, why);
      CloseHandle(heldHelper);
      if (!launched)
      {
         UpdateApplyToken::Revoke();
         UpdateChecker::RecordFailure(why);
         LOG_APPLICATION(Formatter::Format(_T("Update apply failed: {0}"), why));
         error = why;
         return false;
      }

      drain.keep = true;
      UpdateChecker::RecordInstalling();
      LOG_APPLICATION(Formatter::Format(_T("Update: hMailServer {0} is being applied by {1}; the server has drained, the service will stop and start, and the outcome is reported when it does."),
         snapshot.available_version, helper));
      boost::thread(&UpdateInstaller::WatchTheHelper_, drainSince, String()).detach();
      return true;
#endif
#endif
   }

   // ---- An administrator's install -------------------------------------------

   namespace
   {
      // The preparation that runs, and whether it has been told to stop.
      std::atomic<bool> preparation_running_(false);
      std::atomic<bool> preparation_cancelled_(false);

      // The drain BeginForAdministrator entered, which Cancel may leave and no
      // other.
      boost::mutex administrator_drain_mutex_;
      String administrator_drain_since_;

      // A preparation ready for this long, and not begun, goes back.
      const __int64 READY_UNBEGUN_SECONDS = (__int64) 90 * 60;

      // How long an in-progress mark may stand with no outcome before it is
      // reported as an install that did not finish.
      const __int64 ADMINISTRATOR_INSTALL_SILENT_SECONDS = 3600;

      // A JSON string, every character outside printable ASCII written as a JSON escape,
      // so that the document survives the snapshot's String and the ANSI
      // conversion ToJson makes of it byte for byte.
      String JsonText_(const String &value)
      {
         String out = _T("\"");
         for (int i = 0; i < value.GetLength(); i++)
         {
            const unsigned int c = (unsigned int) (unsigned short) value[i];
            if (c == '"')
               out += _T("\\\"");
            else if (c == '\\')
               out += _T("\\\\");
            else if (c < 0x20 || c > 0x7E)
            {
               String escaped;
               escaped.Format(_T("\\u%04x"), c);
               out += escaped;
            }
            else
               out += (TCHAR) c;
         }
         out += _T("\"");
         return out;
      }

      struct PreparationDocument_
      {
         PreparationDocument_() : backupTaken(false), backupRunning(false), rollbackReady(false) {}

         String step;             // check, backup, rollback, ready, failed, cancelled
         bool backupTaken;
         bool backupRunning;
         String backupSkipped;
         String backupFailed;
         String backupPath;
         String rollbackVersion;
         bool rollbackReady;
         String rollbackReason;
         String error;

         String Json() const
         {
            String json = _T("{\"step\":") + JsonText_(step);
            json += _T(",\"backup\":{\"taken\":") + String(backupTaken ? _T("true") : _T("false"));
            json += _T(",\"running\":") + String(backupRunning ? _T("true") : _T("false"));
            json += _T(",\"skipped\":") + JsonText_(backupSkipped);
            json += _T(",\"failed\":") + JsonText_(backupFailed);
            json += _T(",\"path\":") + JsonText_(backupPath) + _T("}");
            json += _T(",\"rollback\":{\"version\":") + JsonText_(rollbackVersion);
            json += _T(",\"ready\":") + String(rollbackReady ? _T("true") : _T("false"));
            json += _T(",\"reason\":") + JsonText_(rollbackReason) + _T("}");
            json += _T(",\"error\":") + JsonText_(error) + _T("}");
            return json;
         }
      };

      String ObjectName_(const String &version)
      {
         return _T("hMailServer ") + version;
      }

      // The backup the preparation takes, as the unattended apply takes it
      // (UpdateCheckTask): started, waited for for up to an hour, and judged.
      bool RunBackup_(String &why)
      {
         std::shared_ptr<BackupManager> manager = Application::Instance()->GetBackupManager();
         if (!manager)
         {
            why = _T("the backup manager is not running");
            return false;
         }
         if (!manager->StartBackup())
         {
            why = _T("another backup or restore is running");
            return false;
         }
         for (int waited = 0; waited < 3600 && manager->IsRunning(); waited += 5)
         {
            if (preparation_cancelled_)
               break;
            boost::this_thread::sleep_for(boost::chrono::seconds(5));
         }
         if (manager->IsRunning())
         {
            why = preparation_cancelled_ ? String(_T("the install was cancelled while the backup ran")) : String(_T("the backup was still running after an hour"));
            return false;
         }
         if (!manager->LastBackupSucceeded())
         {
            why = _T("the backup failed; the backup log says why");
            return false;
         }
         return true;
      }

      String ReadSmallFile_(const String &path, AnsiString &text)
      {
         text.Empty();
#ifdef HM_PLATFORM_POSIX
         (void) path;
         return _T("not on this platform");
#else
         HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (file == INVALID_HANDLE_VALUE)
            return Formatter::Format(_T("{0} could not be opened (Windows error {1})"), path, (int) GetLastError());
         FILE_ATTRIBUTE_TAG_INFO tag;
         memset(&tag, 0, sizeof(tag));
         LARGE_INTEGER size;
         size.QuadPart = 0;
         String why;
         if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
             !GetFileSizeEx(file, &size) || size.QuadPart > 16 * 1024)
            why = Formatter::Format(_T("{0} is a link, or larger than an outcome is"), path);
         else
         {
            std::vector<char> buffer((size_t) size.QuadPart + 1);
            DWORD got = 0;
            if (ReadFile(file, buffer.data(), (DWORD) size.QuadPart, &got, nullptr))
               text = AnsiString(buffer.data(), (int) got);
            else
               why = Formatter::Format(_T("{0} could not be read"), path);
         }
         CloseHandle(file);
         return why;
#endif
      }
   }

   bool
   UpdateInstaller::NeedsAdministrator()
   {
#ifdef HM_PLATFORM_POSIX
      return false;
#else
      return UpdateChecker::DetectInstallKind() == UpdateChecker::InstallWindowsInstaller && !ServiceAccount::ProcessIsAdministrator();
#endif
   }

   String
   UpdateInstaller::ServerName()
   {
#ifdef HM_PLATFORM_POSIX
      return String();
#else
      wchar_t name[512];
      DWORD length = 512;
      if (GetComputerNameExW(ComputerNameDnsFullyQualified, name, &length) && length > 0)
         return String(name);
      length = 512;
      if (GetComputerNameExW(ComputerNameNetBIOS, name, &length) && length > 0)
         return String(name);
      return _T("this server");
#endif
   }

   String
   UpdateInstaller::InstallAdvice()
   {
      if (!NeedsAdministrator())
         return String();

      const String account = ServiceAccount::ProcessAccountName();
      const String version = UpdateChecker::Current().available_version;
      return Formatter::Format(_T("This service runs as {0}, which cannot run an installer, so updates are installed by an administrator at the server: open the hMailServer Control Panel on {1} and click Install update on its Status page. It asks Windows for permission once, checks the installer itself, and installs {2}."),
         account.IsEmpty() ? String(_T("an account that is not an administrator")) : account, ServerName(),
         version.IsEmpty() ? String(_T("the new release")) : _T("hMailServer ") + version);
   }

   bool
   UpdateInstaller::PrepareForAdministrator(const String &version, bool skipBackup, const String &windowsAccount, String &error)
   {
      error.Empty();
      if (!NeedsAdministrator())
      {
         error = _T("This service installs updates itself; Install update hands the installer to its update helper.");
         return false;
      }

      const UpdateChecker::Snapshot snapshot = UpdateChecker::Current();
      const bool downloaded = snapshot.state == UpdateChecker::StateDownloaded;
      const bool retry = snapshot.state == UpdateChecker::StateFailed && !snapshot.preparation.IsEmpty();
      if (!(downloaded || retry) || snapshot.installer_path.IsEmpty() || !FileUtilities::Exists(snapshot.installer_path))
      {
         error = _T("No verified installer is waiting; check for updates and download it first.");
         return false;
      }
      if (version.IsEmpty() || !(version == snapshot.available_version))
      {
         error = Formatter::Format(_T("The installer waiting is for hMailServer {0}, not {1}; reload the Status page."), snapshot.available_version, version);
         return false;
      }
      if (Application::Instance()->IsDraining())
      {
         error = Formatter::Format(_T("The server is draining for maintenance since {0}. Leave the drain, or wait for it to end, and click Install update again."),
            Application::Instance()->GetDrainStatus().since);
         return false;
      }
      if (preparation_running_.exchange(true))
      {
         error = _T("An install of this update is already being prepared.");
         return false;
      }

      preparation_cancelled_ = false;
      PreparationDocument_ document;
      document.step = _T("check");
      UpdateChecker::RecordPreparation(UpdateChecker::StatePreparing, version, document.Json());

      const String backupWord = skipBackup ? String(_T("skipped: the administrator chose to install without one")) : String(_T("first, where one is configured"));
      AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(version), _T("install requested"),
         Formatter::Format(_T("from this computer as Windows account {0}; backup {1}; to be installed after the Windows elevation prompt"),
            windowsAccount, backupWord));
      LOG_APPLICATION(Formatter::Format(_T("Update: {0} asked, from this computer, to prepare hMailServer {1} for an administrator's install: backup {2}; the rollback image for {3} is fetched."),
         windowsAccount, version, backupWord, UpdateChecker::RunningVersion()));

      boost::thread(&UpdateInstaller::PrepareForAdministrator_, version, skipBackup, windowsAccount).detach();
      return true;
   }

   void
   UpdateInstaller::PrepareForAdministrator_(String version, bool skipBackup, String windowsAccount)
   {
      struct Running_
      {
         ~Running_() { preparation_running_ = false; }
      } runningGuard;

      PreparationDocument_ document;
      document.step = _T("check");

      auto fail = [&](const String &step, const String &reason)
      {
         document.step = _T("failed");
         document.error = reason;
         UpdateChecker::RecordPreparationFailed(reason, document.Json());
         LOG_APPLICATION(Formatter::Format(_T("Update: the preparation of hMailServer {0} for an administrator's install failed at {1}: {2}"), version, step, reason));
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7468, "UpdateInstaller::PrepareForAdministrator_",
            Formatter::Format(_T("The preparation of hMailServer {0} for an administrator's install failed at {1}: {2}"), version, step, reason));
      };

#if HM_LIVE_UPDATE
      // 1. The service's own check of the installer it downloaded: a copy that no
      // longer verifies fails here, before a backup is taken for nothing. The
      // administrator's stage checks its own copy again regardless.
      UpdateDownloader::UpdatesFolders folders;
      String error;
      if (!UpdateDownloader::PrepareUpdatesDirectory(folders, error))
      {
         fail(_T("check"), error);
         return;
      }
      SigstoreTrust trust;
      if (!SigstoreVerifier::ConfiguredTrust(trust, error))
      {
         fail(_T("check"), error);
         return;
      }
      const UpdateChecker::Snapshot snapshot = UpdateChecker::Current();
      if (!FileUtilities::Exists(snapshot.installer_path) || !CheckWaitingInstaller_(snapshot, trust, nullptr, error))
      {
         fail(_T("check"), error.IsEmpty() ? String(_T("the installer that was waiting is gone; download it again")) : error + _T(" Download it again."));
         return;
      }
      if (preparation_cancelled_)
         return;

      // 2. The backup, as the unattended apply takes it - but where it cannot be
      // taken the administrator is told, rather than the install refused: they
      // are there, and chose to install.
      document.step = _T("backup");
      const String destination = Configuration::Instance()->GetBackupDestination();
      // No note of a backup for this install until this one has taken it
      // (RecordBackupForHelper_).
      RecordBackupForHelper_(version, String());
      if (skipBackup)
         document.backupSkipped = _T("chosen");
      else if (!IniFileSettings::Instance()->GetUpdateBackupBeforeApply())
         document.backupSkipped = _T("off");
      else if (destination.IsEmpty())
         document.backupSkipped = _T("no-destination");
      else
      {
         document.backupRunning = true;
         document.backupPath = destination;
         UpdateChecker::RecordPreparation(UpdateChecker::StatePreparing, version, document.Json());
         LOG_APPLICATION(Formatter::Format(_T("Update: backing up to {0} before an administrator's install of hMailServer {1}."), destination, version));
         String why;
         const bool taken = RunBackup_(why);
         document.backupRunning = false;
         if (preparation_cancelled_)
            return;
         if (!taken)
         {
            document.backupFailed = why;
            fail(_T("backup"), Formatter::Format(_T("The backup before the update failed: {0}."), why));
            return;
         }
         document.backupTaken = true;
         RecordBackupForHelper_(version, destination);
      }
      UpdateChecker::RecordPreparation(UpdateChecker::StatePreparing, version, document.Json());

      // 3. The rollback image: the running version's installer, fetched and
      // verified as the unattended apply fetches it. None is not fatal.
      document.step = _T("rollback");
      const String running = UpdateChecker::RunningVersion();
      document.rollbackVersion = running;
      UpdateChecker::RecordPreparation(UpdateChecker::StatePreparing, version, document.Json());
      String rollbackPath, rollbackWhy;
      document.rollbackReady = EnsureRollbackImage_(running, rollbackPath, rollbackWhy) &&
                               Check_(rollbackPath, running, trust, nullptr, rollbackWhy) == CheckPassed_;
      if (!document.rollbackReady)
         document.rollbackReason = rollbackWhy;
      if (preparation_cancelled_)
         return;

      // 4. Ready: the Control Panel tells its stage, which checks the rollback
      // image itself, and then begins.
      document.step = _T("ready");
      if (!UpdateChecker::RecordPreparation(UpdateChecker::StateReadyForAdministrator, version, document.Json()))
      {
         LOG_APPLICATION(Formatter::Format(_T("Update: the release on offer changed while hMailServer {0} was prepared; the preparation is dropped."), version));
         return;
      }
      LOG_APPLICATION(Formatter::Format(_T("Update: hMailServer {0} is ready for the administrator's install; backup {1}; rollback image {2}."), version,
         document.backupTaken ? document.backupPath : String(_T("none")),
         document.rollbackReady ? running : _T("none (") + document.rollbackReason + _T(")")));
      (void) windowsAccount;
#else
      (void) skipBackup;
      (void) windowsAccount;
      fail(_T("check"), _T("This build of hMailServer was built without the live update."));
#endif
   }

   bool
   UpdateInstaller::BeginForAdministrator(const String &version, const String &windowsAccount, String &error)
   {
      error.Empty();
      if (!NeedsAdministrator())
      {
         error = _T("This service installs updates itself; Install update hands the installer to its update helper.");
         return false;
      }
      const UpdateChecker::Snapshot snapshot = UpdateChecker::Current();
      if (snapshot.state != UpdateChecker::StateReadyForAdministrator || !(snapshot.available_version == version))
      {
         error = Formatter::Format(_T("hMailServer {0} is not prepared for an administrator's install; click Install update again."), version);
         return false;
      }

#ifdef HM_PLATFORM_POSIX
      error = _T("An administrator's install is a Windows installation's.");
      return false;
#else
      // The token the new installer's database upgrade presents, issued now
      // rather than at the preparation: an hour-long backup cannot outlive it.
      AnsiString token;
      String why;
      if (!UpdateApplyToken::Issue(token, why))
      {
         error = why;
         return false;
      }
      SecureZeroMemory((void *) token.c_str(), (size_t) token.GetLength());

      const String drainRefusal = Application::Instance()->EnterDrain();
      if (!drainRefusal.IsEmpty())
      {
         UpdateApplyToken::Revoke();
         // The Control Panel puts what this is in front of it ("The update was not
         // installed: ..."), as it does for every refusal here.
         error = drainRefusal.CompareNoCase(_T("already draining")) == 0
            ? String(_T("The server is already draining. Leave the drain, or wait for it to end, and click Install update again."))
            : Formatter::Format(_T("The server is {0}, so it could not stop taking new connections for the install. Click Install update again when it is running."), drainRefusal);
         return false;
      }
      const String drainSince = Application::Instance()->GetDrainStatus().since;
      {
         boost::lock_guard<boost::mutex> guard(administrator_drain_mutex_);
         administrator_drain_since_ = drainSince;
      }

      UpdateChecker::RecordInstalling();
      AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(version), _T("install started"),
         Formatter::Format(_T("from this computer as Windows account {0}; the server has drained"), windowsAccount));
      LOG_APPLICATION(Formatter::Format(_T("Update: the administrator's install of hMailServer {0} is starting; the server has drained, and the installer stops and starts the service."), version));
      boost::thread(&UpdateInstaller::WatchAdministratorInstall_, drainSince, version).detach();
      return true;
#endif
   }

   void
   UpdateInstaller::CancelForAdministrator(const String &reason, const String &windowsAccount)
   {
      // Only an administrator's install is cancelled here, and only while there
      // is one: prepared, being prepared, failed in its preparation, or begun and
      // not yet handed over. Anything else - above all LocalSystem's own apply
      // under way, whose token and drain this would take away - is left alone,
      // and nothing is recorded for a cancel that cancelled nothing.
      if (!NeedsAdministrator())
         return;
      const UpdateChecker::Snapshot snapshot = UpdateChecker::Current();
      bool begun = false;
      {
         boost::lock_guard<boost::mutex> guard(administrator_drain_mutex_);
         begun = !administrator_drain_since_.IsEmpty();
      }
      const bool prepared = snapshot.state == UpdateChecker::StatePreparing || snapshot.state == UpdateChecker::StateReadyForAdministrator ||
                            (snapshot.state == UpdateChecker::StateFailed && !snapshot.preparation.IsEmpty());
      if (!prepared && !begun && !preparation_running_)
         return;

      preparation_cancelled_ = true;

#ifndef HM_PLATFORM_POSIX
      UpdateApplyToken::Revoke();
#endif
      String drainSince;
      {
         boost::lock_guard<boost::mutex> guard(administrator_drain_mutex_);
         drainSince = administrator_drain_since_;
         administrator_drain_since_.Empty();
      }
      const Application::DrainStatus drain = Application::Instance()->GetDrainStatus();
      const bool leftDrain = !drainSince.IsEmpty() && drain.draining && drain.since == drainSince;
      if (leftDrain)
         Application::Instance()->LeaveDrainAsTheServer(Formatter::Format(_T("The administrator's install was cancelled: {0}"), reason));

      UpdateChecker::RecordPreparationEnded();
      AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(snapshot.available_version), _T("install cancelled"),
         Formatter::Format(_T("from this computer as Windows account {0}: {1}"), windowsAccount, reason));
      LOG_APPLICATION(Formatter::Format(_T("Update: the administrator's install of hMailServer {0} was cancelled: {1}; the token was revoked{2}."),
         snapshot.available_version, reason, leftDrain ? String(_T(", and the drain left")) : String()));
   }

   void
   UpdateInstaller::ExpireReadyPreparation()
   {
      if (UpdateChecker::RecordReadyTimedOut(READY_UNBEGUN_SECONDS))
         LOG_APPLICATION(_T("Update: an administrator's install was prepared and not begun within 90 minutes; the installer is waiting again, and Install update prepares it afresh."));
   }

   void
   UpdateInstaller::WatchAdministratorInstall_(String drainSince, String version)
   {
#ifndef HM_PLATFORM_POSIX
      const ULONGLONG deadline = GetTickCount64() + (ULONGLONG) 10 * 60 * 1000;
      const String outcomePath = AdminUpdate::LastInstallPath();
      bool seen = false;
      for (;;)
      {
         Sleep(5000);

         Application::DrainStatus drain = Application::Instance()->GetDrainStatus();
         if (!drain.draining || drain.since != drainSince)
            return;

         // An outcome the service has not reported, while it still runs: the
         // install ended without stopping the service - Inno refused before it
         // stopped anything, the Control Panel closed without the go code, the
         // installer was not the file checked. Seen twice, so that the Updater
         // has finished writing it.
         AnsiString text;
         AdministratorOutcome outcome;
         if (!outcomePath.IsEmpty() && FileUtilities::Exists(outcomePath) && ReadSmallFile_(outcomePath, text).IsEmpty() &&
             ParseAdministratorOutcome(text, outcome))
         {
            AnsiString reported;
            ReadSmallFile_(AdminUpdate::ReportedIdPath(), reported);
            reported.Trim();
            if (!(String(reported) == outcome.id))
            {
               if (!seen)
               {
                  seen = true;
                  continue;
               }
               LOG_APPLICATION(Formatter::Format(_T("Update: the administrator's install of hMailServer {0} ended while this service still runs ({1}); the drain is left and the outcome reported."),
                  version, outcome.status));
               UpdateApplyToken::Revoke();
               Application::Instance()->LeaveDrainAsTheServer(_T("The administrator's install ended without stopping the service; its outcome is reported now."));
               // Nothing was installed and this process still runs: the verified
               // installer is waiting again (state 3), so that Install update is
               // offered again, rather than the state the outcome would leave it
               // in - "not checked", which offers neither Install nor the file.
               UpdateChecker::RecordPreparationEnded();
               ReportLastOutcome();
               return;
            }
         }

         if (GetTickCount64() >= deadline)
         {
            UpdateApplyToken::Revoke();
            const String message = Formatter::Format(_T("The administrator's install of hMailServer {0} did not stop the service within 10 minutes, so the drain it entered has been left, the token revoked, and the server accepts again. The Updater's log in {1} says how far it got."),
               version, AdminUpdate::AdminUpdateFolder());
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7465, "UpdateInstaller::WatchAdministratorInstall_", message);
            UpdateChecker::RecordFailure(message);
            Application::Instance()->LeaveDrainAsTheServer(_T("The administrator's install did not stop the service within 10 minutes."));
            return;
         }
      }
#else
      (void) drainSince;
      (void) version;
#endif
   }

   bool
   UpdateInstaller::ParseAdministratorOutcome(const AnsiString &text, AdministratorOutcome &outcome)
   {
      outcome = AdministratorOutcome();
      String all;
      Unicode::MultiByteToWide(text, all);
      std::vector<String> lines = StringParser::SplitString(all, _T("\n"));
      if (lines.empty())
         return false;

      String first = lines[0];
      first.Trim();
      const int space = first.Find(_T(" "));
      if (space <= 0)
         return false;
      outcome.status = first.Left(space);
      String rest = first.Mid(space + 1);
      const int second = rest.Find(_T(" "));
      outcome.version = second > 0 ? rest.Left(second) : rest;
      outcome.detail = second > 0 ? rest.Mid(second + 1) : String();

      const TCHAR *statuses[] = { _T("ok"), _T("failed"), _T("rolled-back"), _T("rollback-failed"), _T("no-rollback"), _T("upgraded-incomplete"), _T("cancelled"), _T("refused") };
      bool known = false;
      for (const TCHAR *status : statuses)
         known = known || outcome.status == status;
      if (!known)
         return false;

      for (size_t i = 1; i < lines.size(); i++)
      {
         String line = lines[i];
         line.Trim();
         const int equals = line.Find(_T("="));
         if (equals <= 0)
            continue;
         const String key = line.Left(equals);
         const String value = line.Mid(equals + 1);
         if (key == _T("id"))
            outcome.id = value;
         else if (key == _T("by"))
            outcome.by = value;
         else if (key == _T("from"))
            outcome.from = value;
         else if (key == _T("install-log"))
            outcome.installLog = value;
      }

      // The id is the work folder's: 32 lowercase hex characters, or it is not
      // an outcome the Updater wrote.
      if (outcome.id.GetLength() != 32)
         return false;
      for (int i = 0; i < outcome.id.GetLength(); i++)
      {
         const TCHAR c = outcome.id[i];
         if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
      }
      return true;
   }

   bool
   UpdateInstaller::AdministratorInstallPending_()
   {
#ifdef HM_PLATFORM_POSIX
      return false;
#else
      const String progress = AdminUpdate::InProgressPath();
      if (progress.IsEmpty() || !FileUtilities::Exists(progress))
         return false;
      AnsiString text, reported;
      if (!ReadSmallFile_(progress, text).IsEmpty())
         return false;
      ReadSmallFile_(AdminUpdate::ReportedIdPath(), reported);
      reported.Trim();
      return text.Find("id=" + reported) < 0 || reported.IsEmpty();
#endif
   }

   void
   UpdateInstaller::ReportAdministratorOutcome_()
   {
#ifndef HM_PLATFORM_POSIX
      const String path = AdminUpdate::LastInstallPath();
      const String reportedPath = AdminUpdate::ReportedIdPath();
      if (path.IsEmpty() || reportedPath.IsEmpty())
         return;

      AnsiString reportedText;
      ReadSmallFile_(reportedPath, reportedText);
      reportedText.Trim();
      const String reported(reportedText);

      AnsiString text;
      AdministratorOutcome outcome;
      if (FileUtilities::Exists(path))
      {
         const String why = ReadSmallFile_(path, text);
         if (!why.IsEmpty() || !ParseAdministratorOutcome(text, outcome))
         {
            static bool said = false;
            if (!said)
            {
               said = true;
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7469, "UpdateInstaller::ReportAdministratorOutcome_",
                  Formatter::Format(_T("The outcome of the last administrator's install, {0}, could not be read: {1}"), path,
                     why.IsEmpty() ? String(_T("it is not an outcome the Updater writes")) : why));
            }
            return;
         }
      }

      if (!outcome.id.IsEmpty() && !(outcome.id == reported))
      {
         FileUtilities::WriteToFile(reportedPath, outcome.id, false);

         const String by = outcome.by.IsEmpty() ? String(_T("an administrator")) : outcome.by;
         UpdateChecker::RecordApplyOutcome(outcome.status, outcome.version, Formatter::Format(_T("{0} (installed by {1})"), outcome.detail, by));

         // The audit row, under the Windows account the elevated Updater read
         // from its own token, into a file only administrators can write.
         AuditTrail::Actor actor;
         actor.present = true;
         actor.name = by;
         actor.kind = _T("administrator");
         actor.interface_name = _T("Windows");
         {
            AuditScope scope(actor);
            const String action = outcome.status == _T("ok") ? String(_T("installed")) : outcome.status == _T("rolled-back") ? String(_T("rolled back"))
                                : outcome.status == _T("cancelled") ? String(_T("install cancelled"))
                                : outcome.status == _T("upgraded-incomplete") ? String(_T("installed incomplete")) : String(_T("install failed"));
            // An install refused or cancelled before the installer ran has no log.
            AuditTrail::Instance()->RecordEvent(_T("update"), ObjectName_(outcome.version), action,
               Formatter::Format(_T("{0} {1}: {2}; from {3}"), outcome.status, outcome.version, outcome.detail, outcome.from) +
                  (outcome.installLog.IsEmpty() ? String() : _T("; log ") + outcome.installLog));
         }

         if (outcome.status == _T("ok"))
         {
            LOG_APPLICATION(Formatter::Format(_T("Update applied by an administrator ({0}): hMailServer {1} is installed and this is it running; {2}."), by, outcome.version, outcome.detail));
         }
         else if (outcome.status == _T("cancelled"))
         {
            LOG_APPLICATION(Formatter::Format(_T("Update: the administrator's install of hMailServer {0} was cancelled ({1}); nothing was installed."), outcome.version, outcome.detail));
         }
         else if (outcome.status == _T("upgraded-incomplete"))
         {
            const String message = Formatter::Format(_T("Update to hMailServer {0} did not complete after its database step had changed the database, so the earlier version was not put back ({1}): {2} (installed by an administrator, {3})"),
               outcome.version, outcome.status, outcome.detail, by);
            LOG_APPLICATION(message);
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5481, "UpdateInstaller::ReportAdministratorOutcome_", message);
         }
         else
         {
            const String message = Formatter::Format(_T("Update to hMailServer {0} did not succeed ({1}): {2} (installed by an administrator, {3})"), outcome.version, outcome.status, outcome.detail, by);
            LOG_APPLICATION(message);
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5480, "UpdateInstaller::ReportAdministratorOutcome_", message);
         }
         return;
      }

      // An install that began and wrote no outcome within the hour - its
      // session signed out, or the machine lost power - said once.
      const String progress = AdminUpdate::InProgressPath();
      AnsiString progressText;
      if (progress.IsEmpty() || !FileUtilities::Exists(progress) || !ReadSmallFile_(progress, progressText).IsEmpty())
         return;
      String progressWide;
      Unicode::MultiByteToWide(progressText, progressWide);
      String id, version, started;
      std::vector<String> lines = StringParser::SplitString(progressWide, _T("\n"));
      for (String line : lines)
      {
         line.Trim();
         if (line.Left(3) == _T("id="))
            id = line.Mid(3);
         else if (line.Left(8) == _T("version="))
            version = line.Mid(8);
         else if (line.Left(8) == _T("started="))
            started = line.Mid(8);
      }
      if (id.IsEmpty() || id == reported)
         return;
      std::time_t written = 0;
      boost::system::error_code timeError;
      written = boost::filesystem::last_write_time(boost::filesystem::path(std::wstring(progress)), timeError);
      if (timeError || std::time(nullptr) - written < ADMINISTRATOR_INSTALL_SILENT_SECONDS)
         return;
      FileUtilities::WriteToFile(reportedPath, id, false);
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7469, "UpdateInstaller::ReportAdministratorOutcome_",
         Formatter::Format(_T("An administrator's install of hMailServer {0} started at {1} did not finish: it wrote no outcome (the Windows session it ran in may have been signed out). The Updater's log in {2} says how far it got."),
            version, started, AdminUpdate::AdminUpdateFolder()));
#endif
   }

   void
   UpdateInstaller::ReportLastOutcome()
   {
      ReportUnattendedOutcome_();
      ReportAdministratorOutcome_();
   }

   void
   UpdateInstaller::ReportUnattendedOutcome_()
   {
      String path = OutcomePath();
      if (path.IsEmpty() || !FileUtilities::Exists(path))
         return;

      String line = FileUtilities::ReadCompleteTextFile(path);
      int newline = line.Find(_T("\n"));
      if (newline >= 0)
         line = line.Left(newline);
      line.TrimRight();
      line.TrimLeft();

      // "<status> <version> <detail>"
      String status, version, detail;
      int first = line.Find(_T(" "));
      if (first > 0)
      {
         status = line.Left(first);
         String rest = line.Mid(first + 1);
         int second = rest.Find(_T(" "));
         if (second > 0)
         {
            version = rest.Left(second);
            detail = rest.Mid(second + 1);
         }
         else
            version = rest;
      }
      else
         status = line;

      String reported = path + _T(".reported");
      if (FileUtilities::Exists(reported))
         FileUtilities::DeleteFile(reported);
      FileUtilities::Move(path, reported);

      UpdateChecker::RecordApplyOutcome(status, version, detail);

      String message;
      if (status == _T("ok"))
      {
         message = Formatter::Format(_T("Update applied: hMailServer {0} is installed and this is it running; {1}."), version, detail);
         LOG_APPLICATION(message);
         return;
      }

      // An update whose database step changed the database and that then did not
      // complete is not rolled back - the earlier version may not run on that
      // database - and has an error of its own, HM5481, for what has to be done
      // about it is not what HM5480's is (docs/Upgrading.md).
      if (status == _T("upgraded-incomplete"))
      {
         message = Formatter::Format(_T("Update to hMailServer {0} did not complete after its database step had changed the database, so the earlier version was not put back ({1}): {2}"),
            version, status, detail);
         LOG_APPLICATION(message);
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5481, "UpdateInstaller::ReportLastOutcome", message);
         return;
      }

      message = Formatter::Format(_T("Update to hMailServer {0} did not succeed ({1}): {2}"), version, status, detail);
      LOG_APPLICATION(message);
      ErrorManager::Instance()->ReportError(ErrorManager::High, 5480, "UpdateInstaller::ReportLastOutcome", message);
   }
}
