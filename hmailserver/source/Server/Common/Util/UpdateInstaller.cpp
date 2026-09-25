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
#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../Application/ErrorManager.h"

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
      const TCHAR *HELPER_NAME = _T("hmailserver-update");
      // Where the .deb and the .rpm install the helper, and where an AppImage
      // carries it relative to its own root.
      const char *PACKAGED_HELPER = "/usr/lib/hmailserver/hmailserver-update";
      const char *APPIMAGE_HELPER = "/usr/lib/hmailserver/hmailserver-update";
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

      // AwaitOutcomeAtStart: how long past the helper's own wait for the service
      // the outcome is looked for, and how recent the helper's log must be for an
      // apply to count as still in progress (the helper gives an installer an
      // hour).
      const int UPDATE_OUTCOME_GRACE_SECONDS = 120;
      const int UPDATE_HELPER_RECENT_SECONDS = 3600;
   }

   void
   UpdateInstaller::WatchTheHelper_(const String &drainSince)
   {
      const ULONGLONG deadline = GetTickCount64() + (ULONGLONG) UPDATE_APPLY_DRAIN_MINUTES * 60 * 1000;
      bool outcomeSeen = false;

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
      return true;
#else
      if (ServiceAccount::ProcessIsAdministrator())
         return true;

      // The copy this server downloaded is NOT offered to be run. It is in the
      // Updates folder of the data directory, which the service itself may
      // write, and "verified" is the service's own word for it: an
      // administrator told to run it elevated would be running whatever a
      // compromised service put there - exactly the trust docs/ServiceAccount.md
      // refuses to a SYSTEM task that installs what the service names. So the
      // administrator is sent to the release itself, and to the signature
      // Windows checks, which the service cannot forge.
      const String account = ServiceAccount::ProcessAccountName();
      why = Formatter::Format(_T("This service runs as {0}, which cannot run an installer - that takes an administrator - so it checks for updates and an administrator installs them (docs/ServiceAccount.md). "),
         account.IsEmpty() ? String(_T("an account that is not an administrator")) : account);
      why += Formatter::Format(_T("Download the installer from the release page ({0}), check that Get-AuthenticodeSignature reports its signature Valid, then run it as an administrator. The copy this server downloaded is in a folder the service can write, so it is not the one to run."),
         releaseUrl.IsEmpty() ? String(_T("the one the update check names")) : releaseUrl);
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

   bool
   UpdateInstaller::EnsureRollbackImage_(const String &runningVersion, String &rollbackPath, String &why)
   {
      rollbackPath.Empty();

      // Apply has made it already (UpdateDownloader::PrepareUpdatesDirectory); made
      // here the same way for any other caller, never with inherited permissions.
      String directory = UpdateDownloader::UpdatesDirectory() + FileUtilities::PathSeparator + _T("rollback");
      String repaired;
      if (!ProtectedFile::EnsureDirectory(directory, repaired, why))
         return false;

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
   bool
   UpdateInstaller::LaunchPosix_(const AnsiString &request, const String &version, const String &helperSource, String &error)
   {
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

         // A unit named for the version, collected when it is done or has
         // failed, so that a second attempt at the same version is not refused
         // for the first one's remains.
         String command = String(systemdRun) + _T(" --unit=hmailserver-update-") + version + _T(" --collect --quiet -- ")
            + Quote_(helper) + _T(" apply --request ") + Quote_(requestFile) + _T(" --updates-dir ") + Quote_(directory);
         LOG_APPLICATION(Formatter::Format(_T("Update: {0}; starting the helper with systemd-run: {1}"), untaken, command));

         ProcessLauncher launcher(command, directory);
         launcher.SetMaximumRunTime(30000);
         unsigned int exitCode = 0;
         if (launcher.Launch(exitCode) && exitCode == 0)
         {
            LOG_APPLICATION(_T("Update: systemd-run started the helper as a unit of its own. It stops this unit, applies, starts it and writes the outcome."));
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
      // off an ordinary path; fallback when it cannot say. A program started by it
      // is the file the handle holds: no link along the way is followed again.
      String FinalPathOf_(HANDLE handle, const String &fallback)
      {
         wchar_t buffer[1024];
         const DWORD length = GetFinalPathNameByHandleW(handle, buffer, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (length == 0 || length >= 1024)
            return fallback;

         std::wstring path(buffer, length);
         if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
            path = L"\\\\" + path.substr(8);
         else if (path.compare(0, 4, L"\\\\?\\") == 0)
            path = path.substr(4);
         return String(path.c_str());
      }
#endif
   }

   bool
   UpdateInstaller::Apply(String &error)
   {
      error.Empty();

      UpdateChecker::Snapshot snapshot = UpdateChecker::Current();
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
      // a file checked below could be another by the time it runs.
      if (!UpdateDownloader::PrepareUpdatesDirectory(error))
      {
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
      request += "unit=" + AnsiString(OwnUnitName_().c_str()) + "\n";
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
      {
         const char *image = getenv("APPIMAGE");
         request += "image=" + AnsiString(image ? image : "") + "\n";
      }

      if (!LaunchPosix_(request, snapshot.available_version, helperSource, why))
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
      boost::thread(&UpdateInstaller::WatchTheHelper_, drainSince).detach();
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
      if (heldHelper == INVALID_HANDLE_VALUE ||
          !SigstoreVerifier::Sha256File(helperSource, sourceDigest, digestError) ||
          !SigstoreVerifier::Sha256File(helper, copyDigest, digestError) ||
          sourceDigest != copyDigest)
      {
         if (heldHelper != INVALID_HANDLE_VALUE)
            CloseHandle(heldHelper);
         UpdateApplyToken::Revoke();
         error = Formatter::Format(_T("The update helper at {0} is not {1} byte for byte, so it was not started."), helper, helperSource);
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update apply failed: {0}"), error));
         return false;
      }

      String command = Quote_(FinalPathOf_(heldHelper, helper));
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
      boost::thread(&UpdateInstaller::WatchTheHelper_, drainSince).detach();
      return true;
#endif
   }

   void
   UpdateInstaller::ReportLastOutcome()
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

      message = Formatter::Format(_T("Update to hMailServer {0} did not succeed ({1}): {2}"), version, status, detail);
      LOG_APPLICATION(message);
      ErrorManager::Instance()->ReportError(ErrorManager::High, 5480, "UpdateInstaller::ReportLastOutcome", message);
   }
}
