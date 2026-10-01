// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "UpdateChecker.h"
#include "HttpsClient.h"
#include "JsonDocument.h"
#include "Time.h"
#include "Unicode.h"
#include "FileUtilities.h"
#include "UpdateWindow.h"
#include "UpdateInstaller.h"
#include "ServiceAccount.h"
#include <time.h>
#include "../Application/IniFileSettings.h"
#include "../Application/Version.h"

#ifdef HM_PLATFORM_POSIX
#include <cstdlib>
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
   // The artefact THIS server could install, by the name the release flow gives it.
   //
   // A Windows server wants the installer; a Linux one wants the package its own
   // package manager installed, or the AppImage it is running from, and the
   // architecture is part of the name because one release carries both. An
   // installation with no artefact returns an empty name, which reads through the
   // rest of this file as "there is nothing here for us" - the check still reports
   // the version, and the download simply has nothing to fetch, which is the truth
   // rather than a failure.
   std::string PlatformInstallerName_(const std::string &tag)
   {
#if defined(HM_PLATFORM_POSIX)
   #if defined(__aarch64__)
      const std::string debianArchitecture = "arm64";
      const std::string rpmArchitecture = "aarch64";
      const std::string machine = "aarch64";
   #elif defined(__x86_64__)
      const std::string debianArchitecture = "amd64";
      const std::string rpmArchitecture = "x86_64";
      const std::string machine = "x86_64";
   #else
      const std::string debianArchitecture;
      const std::string rpmArchitecture;
      const std::string machine;
   #endif

      if (debianArchitecture.empty())
         return std::string();

      switch (UpdateChecker::DetectInstallKind())
      {
      case UpdateChecker::InstallDeb:
         return "hmailserver_" + tag + "_" + debianArchitecture + ".deb";
      case UpdateChecker::InstallRpm:
         return "hmailserver-" + tag + "-1." + rpmArchitecture + ".rpm";
      case UpdateChecker::InstallAppImage:
         // build-appimage.sh's OUTPUT_NAME, with uname -m's spelling.
         return "hMailServer-" + tag + "-" + machine + ".AppImage";
      default:
         // A build tree, a tarball, or Arch (which builds from the PKGBUILD):
         // nothing on the release page replaces what is running.
         return std::string();
      }
#elif defined(_M_ARM64)
      // hMailServerARM64.iss's OutputBaseFilename: the ARM64 installer.
      return "hMailServer-" + tag + "-arm64.exe";
#else
      return "hMailServer-" + tag + "-x64.exe";
#endif
   }
}

   namespace
   {
      // Where the project's container image is published (the Dockerfile's
      // header, docs/LiveUpdateKinds.md); a container's check names it with the
      // release's version as the tag.
      const char *CONTAINER_IMAGE = "registry.gitlab.com/hmailserver/hmailserver";

#if defined(HM_PLATFORM_POSIX)
      // A variable of the environment the server was started in, empty when it
      // is not set. The marks of where the server runs - HM_INSTALL_KIND, SNAP,
      // SNAP_INSTANCE_NAME and SNAP_NAME - are read here and nowhere else, each
      // copied at once and then only compared (SNAP with this program's own
      // path, below) or held to a character set: none is opened, written to a
      // buffer or put on a command line. Whoever starts the server sets them,
      // and the most a false one can do is name another way of updating it and
      // keep this server from fetching a release file.
      std::string EnvironmentValue_(const char *name)
      {
         const char *value = getenv(name);
         return value ? std::string(value) : std::string();
      }

      // Whether snapd started this program: SNAP names the snap's mounted
      // revision, and the program's own file (/proc/self/exe, which the kernel
      // keeps) is inside it. SNAP alone is not enough, because a process passes
      // its environment on: a terminal that is itself a snap gives its SNAP to
      // everything started from it, and a server started by hand from there -
      // an AppImage, a build tree - is not that snap and is not updated by it.
      bool RunsInSnap_()
      {
         std::string snap = EnvironmentValue_("SNAP");
         while (snap.size() > 1 && snap[snap.size() - 1] == '/')
            snap.erase(snap.size() - 1);
         if (snap.size() < 2 || snap[0] != '/')
            return false;

         char self[4096] = { 0 };
         const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
         if (length <= 0)
            return false;
         const std::string program(self, (size_t) length);
         return program.size() > snap.size() && program.compare(0, snap.size(), snap) == 0 && program[snap.size()] == '/';
      }
#endif

      // The snap's name as snap refresh takes it: SNAP_INSTANCE_NAME, which is
      // the name with the instance key of a parallel install ("hmailserver_test"),
      // or SNAP_NAME; both are set by snapd. A snap name is lower-case letters,
      // digits and hyphens, beginning with a letter or a digit, and the instance
      // key follows an underscore; anything else is not snapd's word - nor put
      // in front of an administrator as the argument of a command, where a
      // leading hyphen would read as an option - and the snap's own name is used.
      std::string SnapName_()
      {
#if defined(HM_PLATFORM_POSIX)
         const char *variables[2] = { "SNAP_INSTANCE_NAME", "SNAP_NAME" };
         for (const char *variable : variables)
         {
            const std::string name = EnvironmentValue_(variable);
            if (name.empty())
               continue;
            bool plain = name.size() <= 64 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'));
            for (size_t i = 0; plain && i < name.size(); i++)
            {
               const char c = name[i];
               plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            }
            if (plain)
               return name;
         }
#endif
         return "hmailserver";
      }
   }

   UpdateChecker::InstallKind
   UpdateChecker::DetectInstallKind()
   {
#if defined(HM_PLATFORM_POSIX)
      // What is installed decides, not a build-time guess: the same binary runs
      // on Debian and on Fedora, and out of an AppImage on either.
      //
      // The container first, by the image's own mark: the image installs the
      // .deb, so dpkg's record below is there too, and a container is updated by
      // pulling the next image, never by installing a package into this one -
      // whose changes go with the container, and whose server runs as its own
      // user with no root to install anything. Any other value is not a kind
      // this server knows, and the detection goes on as without it.
      std::string kind = EnvironmentValue_("HM_INSTALL_KIND");
      for (size_t i = 0; i < kind.size(); i++)
         kind[i] = (char) tolower((unsigned char) kind[i]);
      if (kind == "container")
         return InstallContainer;

      // Then a snap: snapd sets SNAP to the snap's mounted revision for every
      // process of a snap, and snapd alone replaces that revision - when this
      // program is inside it, and SNAP was not merely inherited (RunsInSnap_).
      if (RunsInSnap_())
         return InstallSnap;

#if !HM_LIVE_UPDATE
      // A build without the live update is a distribution's, or a store's, or
      // somebody's own: whatever package manager recorded it - dpkg's record of
      // a Debian build is the same file as the project's own .deb leaves - it
      // updates it, and no release file of the project's may replace it.
      return InstallDistribution;
#else
      // The AppImage runtime sets APPIMAGE to the image's own path before it
      // runs AppRun, and AppRun execs the server with that environment intact.
      const char *appimage = getenv("APPIMAGE");
      if (appimage && *appimage)
         return InstallAppImage;

      // dpkg records every file it installed for a package in
      // /var/lib/dpkg/info/<package>.list; the file exists exactly while the
      // package is installed, so it is the record of ownership and not a guess
      // from "dpkg is on this machine" - a Fedora box with dpkg installed for
      // building packages must not be told to install a .deb.
      if (access("/var/lib/dpkg/info/hmailserver.list", F_OK) == 0)
         return InstallDeb;

      // rpm keeps its database in a format that has changed twice, so the query
      // is asked of rpm itself, once, and only where rpm is the package manager
      // rather than merely present: a Debian machine has no /var/lib/rpm and an
      // rpm-based one has no /var/lib/dpkg/status.
      if (access("/usr/bin/rpm", X_OK) == 0 && access("/var/lib/dpkg/status", F_OK) != 0)
      {
         static int rpmOwnsThis = -1;
         if (rpmOwnsThis < 0)
         {
            // The shell would have run a program named by the environment;
            // this is the one binary, by its full path, with one argument.
            const int status = system("/usr/bin/rpm -q hmailserver >/dev/null 2>&1");
            rpmOwnsThis = (status == 0) ? 1 : 0;
         }
         if (rpmOwnsThis == 1)
            return InstallRpm;
      }

      return InstallNone;
#endif
#else
      return InstallWindowsInstaller;
#endif
   }

   const char *
   UpdateChecker::InstallKindName(InstallKind kind)
   {
      switch (kind)
      {
      case InstallWindowsInstaller: return "installer";
      case InstallDeb: return "deb";
      case InstallRpm: return "rpm";
      case InstallAppImage: return "appimage";
      case InstallSnap: return "snap";
      case InstallContainer: return "container";
      case InstallDistribution: return "distribution";
      case InstallNone:
      default:
         return "";
      }
   }

   bool
   UpdateChecker::LiveUpdateApplies(InstallKind kind)
   {
#if HM_LIVE_UPDATE
      switch (kind)
      {
      case InstallWindowsInstaller:
      case InstallDeb:
      case InstallRpm:
      case InstallAppImage:
         return true;
      default:
         return false;
      }
#else
      (void) kind;
      return false;
#endif
   }

   bool
   UpdateChecker::UpdatedElsewhere(InstallKind kind, const String &version, String &command, String &sentence)
   {
      command.Empty();
      sentence.Empty();

      switch (kind)
      {
      case InstallSnap:
      {
         const String name = String(SnapName_().c_str());
         command = _T("snap refresh ") + name;
         sentence = Formatter::Format(_T("This server runs as the snap {0}, which snapd updates: snapd refreshes its snaps on its own several times a day, and {1} does it now. The server downloads and installs no release file itself."),
            name, command);
         return true;
      }
      case InstallContainer:
      {
         // The version is a tag ReadRelease_ has already held to letters,
         // digits, dots, dashes and underscores, not starting with a dot or a
         // dash - which is what an image tag may be.
         const String image = String(CONTAINER_IMAGE) + _T(":") + (version.IsEmpty() ? String(_T("latest")) : version);
         command = _T("docker pull ") + image;
         sentence = Formatter::Format(_T("This server runs in a container, which is updated by replacing its image, not by installing anything into it: pull the new image ({0}) and recreate the container from it (docker compose pull and docker compose up -d, or the image tag in the Helm values); the data, log and configuration volumes carry over. The server downloads and installs no release file itself."),
            command);
         return true;
      }
      case InstallDistribution:
         command = String(HM_UPDATE_COMMAND);
         command.TrimLeft();
         command.TrimRight();
         if (command.IsEmpty())
            sentence = _T("This build of hMailServer was built without the live update, so it downloads and installs no release file: update it with the package manager, or from the source, it was installed from.");
         else
            sentence = Formatter::Format(_T("This build of hMailServer is updated by the package manager it was installed with, not by the server itself: {0}. It was built without the live update, so it downloads and installs no release file."),
               command);
         return true;
      default:
         return false;
      }
   }

   String
   UpdateChecker::PlatformInstallerName(const String &version)
   {
      return String(PlatformInstallerName_(std::string(AnsiString(version).c_str())).c_str());
   }

   String
   UpdateChecker::MissingArtefactDescription(const String &version)
   {
#if defined(HM_PLATFORM_POSIX)
      String name = PlatformInstallerName(version);
      if (name.IsEmpty())
         return _T("package this server could install: it was not installed from a .deb, an .rpm or an AppImage");
      return name;
#else
      (void) version;
#if defined(_M_ARM64)
      return _T("ARM64 installer");
#else
      return _T("x64 installer");
#endif
#endif
   }

   namespace
   {
      // The project's own feed: static files GitLab Pages serves from the project
      // (build/generate-update-feed.py, docs/UpdateFeed.md), in the GitHub Releases
      // API's shape that ReadRelease_ reads. releases/latest is the newest stable
      // release as one object; releases.json is every release as a list, which the
      // pre-release channel reads; RELEASES_BASE + /tags/v<version> is one release,
      // which the rollback reads. Each is a file on the site, answered 200 with no
      // redirect - HttpsClient::Request follows none, and Pages answers the bare
      // /releases, a directory there, with a 302.
      const char *STABLE_FEED = "https://updates.progressiverobot.com/releases/latest";
      const char *PRERELEASE_FEED = "https://updates.progressiverobot.com/releases.json";
      const char *RELEASES_BASE = "https://updates.progressiverobot.com/releases";

      // A release list with ten entries and their notes is a few hundred kilobytes;
      // a feed that sends more than this is not the one this reader is for.
      const size_t MAX_FEED_BYTES = 4 * 1024 * 1024;

      // The feed is UTF-8, as JSON is; a release title can carry any character.
      String FromUtf8_(const std::string &text)
      {
         String wide;
         Unicode::MultiByteToWide(AnsiString(text), wide);
         return wide;
      }

      // Up to four numeric components, from the first digit after an optional "v",
      // until the first character that is neither a digit nor a dot.
      void ParseVersion_(const String &text, int components[4])
      {
         for (int i = 0; i < 4; i++)
            components[i] = 0;

         int position = 0;
         if (text.GetLength() > 0 && (text[0] == 'v' || text[0] == 'V'))
            position = 1;

         int index = 0;
         while (position < text.GetLength() && index < 4)
         {
            TCHAR c = text[position];
            if (c >= '0' && c <= '9')
            {
               // Capped, so that a component built to overflow an int cannot.
               if (components[index] < 100000000)
                  components[index] = components[index] * 10 + (c - '0');
               position++;
            }
            else if (c == '.')
            {
               index++;
               position++;
            }
            else
               break;
         }
      }

      // A suffix is anything in a version but digits and dots - "-rc1", "rc1",
      // "_rc1", ".rc1" - and a version with one is a pre-release. Not only a dash:
      // ReadRelease_ accepts letters, underscores and dots in a tag, and the
      // version comparison stops at the first of them, so "6.3.5rc1" would
      // otherwise compare as 6.3.5 and pass for the final release.
      bool HasSuffix_(const String &version)
      {
         for (int i = 0; i < version.GetLength(); i++)
         {
            TCHAR c = version[i];
            if (!((c >= '0' && c <= '9') || c == '.'))
               return true;
         }
         return false;
      }
   }

   boost::mutex UpdateChecker::mutex_;
   UpdateChecker::Snapshot UpdateChecker::snapshot_;

   UpdateChecker::Snapshot
   UpdateChecker::Current()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return snapshot_;
   }

   AnsiString
   UpdateChecker::FeedUrl()
   {
      String configured = IniFileSettings::Instance()->GetUpdateFeedUrl();
      if (!configured.IsEmpty())
         return AnsiString(configured);

      return IsPreReleaseChannel() ? PRERELEASE_FEED : STABLE_FEED;
   }

   bool
   UpdateChecker::IsPreReleaseChannel()
   {
      String channel = IniFileSettings::Instance()->GetUpdateChannel();
      return channel.CompareNoCase(_T("prerelease")) == 0 || channel.CompareNoCase(_T("pre-release")) == 0;
   }

   String
   UpdateChecker::RunningVersion()
   {
      return HMAILSERVER_VERSION;
   }

   int
   UpdateChecker::CompareVersions(const String &left, const String &right)
   {
      int a[4];
      int b[4];
      ParseVersion_(left, a);
      ParseVersion_(right, b);

      for (int i = 0; i < 4; i++)
      {
         if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
      }

      return 0;
   }

   const char *
   UpdateChecker::StateName(State state)
   {
      switch (state)
      {
      case StateUpToDate: return "up-to-date";
      case StateAvailable: return "available";
      case StateDownloaded: return "downloaded";
      case StateInstalling: return "installing";
      case StateFailed: return "failed";
      case StatePreparing: return "preparing";
      case StateReadyForAdministrator: return "ready-for-administrator";
      case StateNotChecked:
      default:
         return "not-checked";
      }
   }

   bool
   UpdateChecker::CheckNow(String &error)
   {
      error.Empty();

      AnsiString url = FeedUrl();
      bool preReleaseChannel = IsPreReleaseChannel();

      // No header of GitHub's API: the feed is static files on GitLab Pages, which
      // read none. Up to 6.3.3 this sent X-GitHub-Api-Version to api.github.com.
      std::vector<AnsiString> headers;

      HttpsClient::Response response;
      String requestError;
      if (!HttpsClient::Request("GET", url, headers, "", "", response, requestError, 20, MAX_FEED_BYTES))
         return Fail_(Formatter::Format(_T("The update feed could not be read: {0}"), requestError), error);

      if (response.status_code != 200)
         return Fail_(Formatter::Format(_T("The update feed answered HTTP {0}."), response.status_code), error);

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(response.body), document, parseError))
         return Fail_(Formatter::Format(_T("The update feed is not JSON: {0}."), String(parseError.c_str())), error);

      Release release;
      String selectError;
      if (!SelectRelease_(document, preReleaseChannel, release, selectError))
         return Fail_(selectError, error);

      String running = RunningVersion();
      const TCHAR *channelName = preReleaseChannel ? _T("pre-release") : _T("stable");

      Snapshot snapshot;
      snapshot.last_checked = Time::GetCurrentDateTime();
      snapshot.last_check_unix = (__int64) time(nullptr);

      String message;
      if (!release.found)
      {
         snapshot.state = StateUpToDate;
         message = Formatter::Format(_T("Update check: this server runs {0}; the feed's only release is a pre-release and the channel is {1}."),
            running, String(channelName));
      }
      else if (CompareVersions(release.version, running) > 0)
      {
         snapshot.state = StateAvailable;
         snapshot.available_version = release.version;
         snapshot.release_name = release.name;
         snapshot.published_at = release.published_at;
         snapshot.release_url = release.url;
         snapshot.installer_name = release.installer_name;
         snapshot.installer_url = release.installer_url;
         snapshot.installer_size = release.installer_size;
         snapshot.installer_digest = release.installer_digest;
         snapshot.bundle_url = release.bundle_url;

         message = Formatter::Format(_T("Update check: hMailServer {0} is available (published {1}); this server runs {2}. {3}"),
            release.version, release.published_at, running, release.url);
         // A snap, a container or a distribution's build names what updates it;
         // its installer_name is empty (PlatformInstallerName_ has no artefact
         // for these kinds), so nothing was taken from the release to download.
         String command, sentence;
         if (UpdatedElsewhere(DetectInstallKind(), release.version, command, sentence))
            message += _T(" ") + sentence;
         else if (release.installer_url.IsEmpty())
            message += Formatter::Format(_T(" The release carries no {0}, so there is nothing to download."), MissingArtefactDescription(release.version));
      }
      else
      {
         snapshot.state = StateUpToDate;
         message = Formatter::Format(_T("Update check: this server runs {0}, which is the latest release on the {1} channel ({2})."),
            running, String(channelName), release.version);
      }

      {
         boost::lock_guard<boost::mutex> guard(mutex_);
         // A verified installer of the same version survives the re-check that
         // found it still current; a different version, or a file that has gone,
         // starts again from available. So does an install of it under way: an
         // administrator's install being prepared or ready (a backup can take an
         // hour, and the scheduled check can run in it), or one handed over.
         const bool underWay = snapshot_.state == StateDownloaded || snapshot_.state == StatePreparing ||
                               snapshot_.state == StateReadyForAdministrator || snapshot_.state == StateInstalling;
         if (snapshot.state == StateAvailable && underWay &&
             snapshot_.available_version == snapshot.available_version && FileUtilities::Exists(snapshot_.installer_path))
         {
            snapshot.state = snapshot_.state;
            snapshot.installer_path = snapshot_.installer_path;
            snapshot.signer_identity = snapshot_.signer_identity;
            snapshot.integrated_time = snapshot_.integrated_time;
            snapshot.verified_at = snapshot_.verified_at;
            snapshot.installing_since_unix = snapshot_.installing_since_unix;
            snapshot.preparation = snapshot_.preparation;
            snapshot.ready_since_unix = snapshot_.ready_since_unix;
         }
         // The last apply's outcome is not the check's to forget. It is read at
         // start-up, a moment before the start-up check, and 6.3.4 and 6.3.5 lost it
         // there: after an update Status.UpdateApplyOutcome, the REST route and the
         // Control Panel said nothing, and only the log had it. It stays until the
         // service stops or the next apply's outcome replaces it.
         snapshot.apply_status = snapshot_.apply_status;
         snapshot.apply_version = snapshot_.apply_version;
         snapshot.apply_detail = snapshot_.apply_detail;
         snapshot_ = snapshot;
      }

      LOG_APPLICATION(message);
      return true;
   }

   bool
   UpdateChecker::RecordDownloaded(const String &path, const String &version, const AnsiString &identity, __int64 integratedTime)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      if (version.IsEmpty() || !(snapshot_.available_version == version))
         return false;
      snapshot_.state = StateDownloaded;
      snapshot_.installer_path = path;
      snapshot_.signer_identity = identity;
      snapshot_.integrated_time = integratedTime;
      snapshot_.verified_at = Time::GetCurrentDateTime();
      snapshot_.last_error.Empty();
      return true;
   }

   void
   UpdateChecker::RecordFailure(const String &reason)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      // The failure replaces the verdict but not what the last successful check
      // learned: a release that was available before the feed went quiet is still
      // available, and the Control Panel can keep saying so beside the error.
      snapshot_.state = StateFailed;
      snapshot_.last_error = reason;
   }

   void
   UpdateChecker::RecordLastError(const String &reason)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.last_error = reason;
   }

   void
   UpdateChecker::RecordInstalling()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.state = StateInstalling;
      snapshot_.installing_since_unix = (__int64) time(nullptr);
      snapshot_.last_error.Empty();
   }

   bool
   UpdateChecker::RecordPreparation(State state, const String &version, const String &preparation)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      if (version.IsEmpty() || !(snapshot_.available_version == version))
         return false;
      snapshot_.state = state;
      snapshot_.preparation = preparation;
      snapshot_.ready_since_unix = state == StateReadyForAdministrator ? (__int64) time(nullptr) : 0;
      snapshot_.last_error.Empty();
      return true;
   }

   void
   UpdateChecker::RecordPreparationFailed(const String &reason, const String &preparation)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.state = StateFailed;
      snapshot_.last_error = reason;
      snapshot_.preparation = preparation;
      snapshot_.ready_since_unix = 0;
   }

   void
   UpdateChecker::RecordPreparationEnded()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.preparation.Empty();
      snapshot_.ready_since_unix = 0;
      if (snapshot_.state == StatePreparing || snapshot_.state == StateReadyForAdministrator || snapshot_.state == StateInstalling ||
          snapshot_.state == StateFailed)
         snapshot_.state = !snapshot_.installer_path.IsEmpty() && FileUtilities::Exists(snapshot_.installer_path) ? StateDownloaded : StateAvailable;
      snapshot_.installing_since_unix = 0;
   }

   bool
   UpdateChecker::RecordReadyTimedOut(__int64 allowedSeconds)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      if (snapshot_.state != StateReadyForAdministrator || snapshot_.ready_since_unix == 0)
         return false;
      if ((__int64) time(nullptr) - snapshot_.ready_since_unix < allowedSeconds)
         return false;
      snapshot_.state = !snapshot_.installer_path.IsEmpty() && FileUtilities::Exists(snapshot_.installer_path) ? StateDownloaded : StateAvailable;
      snapshot_.preparation.Empty();
      snapshot_.ready_since_unix = 0;
      return true;
   }

   void
   UpdateChecker::RecordApplyOutcome(const String &status, const String &version, const String &detail)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.apply_status = status;
      snapshot_.apply_version = version;
      snapshot_.apply_detail = detail;
      // An outcome means the helper has finished: whatever the state said before
      // the restart, the service is running now, and the next check decides.
      if (snapshot_.state == StateInstalling)
         snapshot_.state = StateNotChecked;
      snapshot_.installing_since_unix = 0;
   }

   bool
   UpdateChecker::RecordInstallingTimedOut(__int64 allowedSeconds, const String &reason)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      if (snapshot_.state != StateInstalling || snapshot_.installing_since_unix == 0)
         return false;
      if ((__int64) time(nullptr) - snapshot_.installing_since_unix < allowedSeconds)
         return false;
      // The snapshot lives in this process. A helper that got as far as stopping
      // the service would have ended it, and the process that came back would
      // start with a fresh snapshot and read the outcome file; that this one is
      // still here, still installing, means the helper never got that far.
      snapshot_.state = StateFailed;
      snapshot_.last_error = reason;
      snapshot_.installing_since_unix = 0;
      return true;
   }

   AnsiString
   UpdateChecker::ReleaseByTagUrl(const String &version)
   {
      std::string configured = std::string(AnsiString(IniFileSettings::Instance()->GetUpdateFeedUrl()).c_str());
      std::string tag = "/tags/v" + std::string(AnsiString(version).c_str());
      if (configured.empty())
         return AnsiString((std::string(RELEASES_BASE) + tag).c_str());
      size_t releases = configured.find("/releases");
      if (releases == std::string::npos)
         return AnsiString((configured + tag).c_str());
      return AnsiString((configured.substr(0, releases + 9) + tag).c_str());
   }

   bool
   UpdateChecker::FetchReleaseByTag(const String &version, ReleaseAssets &assets, String &error)
   {
      assets = ReleaseAssets();

      // No X-GitHub-Api-Version: the feed is GitLab Pages (see CheckNow).
      std::vector<AnsiString> headers;

      HttpsClient::Response response;
      String requestError;
      AnsiString url = ReleaseByTagUrl(version);
      if (!HttpsClient::Request("GET", url, headers, "", "", response, requestError, 20, MAX_FEED_BYTES))
      {
         error = Formatter::Format(_T("the release feed could not be read for {0}: {1}"), version, requestError);
         return false;
      }
      if (response.status_code != 200)
      {
         error = Formatter::Format(_T("the release feed answered HTTP {0} for release {1}"), response.status_code, version);
         return false;
      }

      JsonValue document;
      std::string parseError;
      Release release;
      if (!JsonValue::Parse(std::string(response.body), document, parseError) || !ReadRelease_(document, release))
      {
         error = Formatter::Format(_T("the feed's entry for release {0} is not a release"), version);
         return false;
      }

      assets.version = release.version;
      assets.installer_url = release.installer_url;
      assets.installer_size = release.installer_size;
      assets.installer_digest = release.installer_digest;
      assets.bundle_url = release.bundle_url;
      return true;
   }

   __int64
   UpdateChecker::NextCheckUnix(const Snapshot &snapshot)
   {
      if (snapshot.last_check_unix == 0)
         return 0;
      __int64 interval = (__int64) IniFileSettings::Instance()->GetUpdateCheckHours() * 3600;
      // One retry sooner after a check that failed, then the usual interval again: a
      // feed that is down stays asked once in UpdateCheckHours, and each failure is a
      // line in the log.
      if (snapshot.check_failures == 1 && interval > CHECK_RETRY_SECONDS)
         interval = CHECK_RETRY_SECONDS;
      return snapshot.last_check_unix + interval;
   }

   bool
   UpdateChecker::CheckIsDue()
   {
      Snapshot snapshot = Current();
      if (snapshot.last_check_unix == 0)
         return true;
      return (__int64) time(nullptr) >= NextCheckUnix(snapshot);
   }

   void
   UpdateChecker::ResetSchedule()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      snapshot_.last_check_unix = 0;
      // And the failures counted: a start is a new try, whose first failure has
      // its retry too.
      snapshot_.check_failures = 0;
   }

   void
   UpdateChecker::WindowStatus(String &text, bool &open, String &error)
   {
      String configured = IniFileSettings::Instance()->GetUpdateWindow();
      UpdateWindow window;
      open = false;
      error.Empty();
      if (!window.Parse(configured, error))
      {
         text = configured;
         return;
      }
      text = window.Describe();
      open = window.ContainsNow();
   }

   String
   UpdateChecker::FormatUnixTime(__int64 seconds)
   {
      if (seconds <= 0)
         return _T("");
      time_t value = (time_t) seconds;
      struct tm parts;
      if (gmtime_s(&parts, &value) != 0)
         return _T("");
      String text;
      text.Format(_T("%04d-%02d-%02dT%02d:%02d:%02dZ"), parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
      return text;
   }

   bool
   UpdateChecker::Fail_(const String &reason, String &error)
   {
      error = reason;
      RecordFailure(reason);

      int failures = 0;
      {
         boost::lock_guard<boost::mutex> guard(mutex_);
         snapshot_.last_checked = Time::GetCurrentDateTime();
         snapshot_.last_check_unix = (__int64) time(nullptr);
         failures = ++snapshot_.check_failures;
      }

      // The first failure in a row is due again in ten minutes (NextCheckUnix), which
      // the scheduled task, running every quarter hour, reads at its first run from
      // then; the line says so where the scheduled check is on.
      String retry;
      if (failures == 1 && IniFileSettings::Instance()->GetUpdateCheckEnabled())
         retry = Formatter::Format(_T(" The scheduled check tries again at its first run {0} minutes or more from now."), (int) (CHECK_RETRY_SECONDS / 60));
      LOG_APPLICATION(Formatter::Format(_T("Update check failed: {0}{1}"), reason, retry));
      return false;
   }

   bool
   UpdateChecker::IsReleasePageUrl(const std::string &url)
   {
      static const char scheme[] = "https://";
      const size_t schemeLength = sizeof(scheme) - 1;
      if (url.size() <= schemeLength || url.size() > 2048)
         return false;

      // The scheme in any case, as a browser reads it.
      for (size_t i = 0; i < schemeLength; i++)
      {
         if (tolower(static_cast<unsigned char>(url[i])) != scheme[i])
            return false;
      }

      // A host after it, not a path, a query, a fragment or an empty one.
      const char first = url[schemeLength];
      if (first == '/' || first == '\\' || first == '?' || first == '#' || first == '@' || first == ':')
         return false;

      // Nothing a browser would drop or read differently: no space, no control
      // character (a browser removes a tab or a line break inside an address).
      for (size_t i = 0; i < url.size(); i++)
      {
         const unsigned char c = static_cast<unsigned char>(url[i]);
         if (c <= 0x20 || c == 0x7f)
            return false;
      }

      return true;
   }

   bool
   UpdateChecker::ReadRelease_(const JsonValue &value, Release &release)
   {
      release = Release();

      if (!value.IsObject())
         return false;

      std::string tag = value.GetString("tag_name");
      if (tag.empty())
         return false;

      if (tag[0] == 'v' || tag[0] == 'V')
         tag = tag.substr(1);

      // The tag is text from the feed, and the installer's file name is built from
      // it below - so it reaches a path under Data\Updates. A feed is trusted for
      // nothing: the release it names is verified against a signature before it is
      // run, and the name it arrives under has to be as bounded as the bytes are.
      // A tag carrying a separator or a traversal would otherwise write the
      // installer, its bundle and its .partial wherever the tag pointed, under the
      // service account. Refused rather than sanitised, because a tag this server
      // cannot spell is a release it cannot install, and saying so is the honest
      // outcome; a real tag is a version number.
      if (tag.size() > 64 ||
          tag.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos ||
          tag[0] == '.' || tag[0] == '-')
         return false;

      release.found = true;
      release.version = FromUtf8_(tag);
      release.name = FromUtf8_(value.GetString("name"));
      // The release page is shown as a link, so it is kept only as what a
      // release page is (IsReleasePageUrl); any other value is dropped rather
      // than the release refused, which then has no page.
      const std::string page = value.GetString("html_url");
      release.url = IsReleasePageUrl(page) ? FromUtf8_(page) : String();
      release.published_at = FromUtf8_(value.GetString("published_at"));
      release.draft = value.GetBool("draft");
      release.prerelease = value.GetBool("prerelease");

      // One release carries a separate artefact for every platform it supports, all
      // at the same version. A server looks for the one it could actually install:
      // the Windows installer here, the package this distribution and architecture
      // uses there. Every one has its Sigstore bundle beside it under the same name
      // and .cosign.bundle, so the verification is identical whichever was chosen.
      //
      // The names are the release flow's, not a guess: RELEASE.md fixes them and
      // sign-release.yml checks the Windows one on every release.
      std::string installerName = PlatformInstallerName_(tag);
      std::string bundleName = installerName + ".cosign.bundle";

      const JsonValue *assets = value.Get("assets");
      if (assets && assets->IsArray())
      {
         for (size_t i = 0; i < assets->Size(); i++)
         {
            const JsonValue *asset = assets->At(i);
            if (!asset || !asset->IsObject())
               continue;

            std::string name = asset->GetString("name");
            if (name == installerName)
            {
               release.installer_name = FromUtf8_(name);
               release.installer_url = FromUtf8_(asset->GetString("browser_download_url"));
               release.installer_size = asset->GetInt64("size");
               release.installer_digest = FromUtf8_(asset->GetString("digest"));
            }
            else if (name == bundleName)
            {
               release.bundle_url = FromUtf8_(asset->GetString("browser_download_url"));
            }
         }
      }

      return true;
   }

   bool
   UpdateChecker::SelectRelease_(const JsonValue &document, bool preReleaseChannel, Release &release, String &error)
   {
      release = Release();

      std::vector<const JsonValue *> candidates;
      if (document.IsObject())
         candidates.push_back(&document);
      else if (document.IsArray())
      {
         for (size_t i = 0; i < document.Size(); i++)
            candidates.push_back(document.At(i));
      }
      else
      {
         error = _T("The update feed is neither a release nor a list of releases.");
         return false;
      }

      bool anyRelease = false;
      Release best;
      for (size_t i = 0; i < candidates.size(); i++)
      {
         Release candidate;
         if (!ReadRelease_(*candidates[i], candidate))
            continue;

         anyRelease = true;

         // A draft is not published; a pre-release is published to those who asked
         // for it. The stable channel is everyone else. A version with a suffix
         // (6.3.5-rc1, 6.3.5rc1) is a pre-release whatever the feed's flag says:
         // the flag is the feed's word, and a genuine pre-release signed for its
         // own tag would pass every check on its way to a stable server whose feed
         // called it stable. The project's feed marks every suffixed tag a
         // pre-release anyway.
         if (candidate.draft)
            continue;
         if ((candidate.prerelease || HasSuffix_(candidate.version)) && !preReleaseChannel)
            continue;

         if (!best.found || CompareVersions(candidate.version, best.version) > 0)
            best = candidate;
      }

      if (!anyRelease)
      {
         error = _T("The update feed has no release in it.");
         return false;
      }

      release = best;
      return true;
   }

   AnsiString
   UpdateChecker::Quote_(const String &value)
   {
      // UTF-8, quoted for JSON: the two characters JSON requires escaped, and the
      // controls, which a feed's release title could carry and a reader must not
      // receive raw.
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);

      AnsiString result = "\"";
      for (int i = 0; i < utf8.GetLength(); i++)
      {
         unsigned char c = (unsigned char) utf8[i];
         if (c == '"')
            result += "\\\"";
         else if (c == '\\')
            result += "\\\\";
         else if (c < 0x20)
         {
            char buffer[8];
            sprintf_s(buffer, sizeof(buffer), "\\u%04x", c);
            result += buffer;
         }
         else
            result += (char) c;
      }
      result += "\"";
      return result;
   }

   AnsiString
   UpdateChecker::ToJson(const Snapshot &snapshot)
   {
      AnsiString size;
      size.Format("%I64d", snapshot.installer_size);

      AnsiString state;
      state.Format("%d", (int) snapshot.state);

      AnsiString body = "{";
      body += "\"state\":" + state;
      body += ",\"stateName\":\"" + AnsiString(StateName(snapshot.state)) + "\"";
      body += ",\"runningVersion\":" + Quote_(RunningVersion());
      body += ",\"channel\":\"" + AnsiString(IsPreReleaseChannel() ? "prerelease" : "stable") + "\"";
      body += ",\"checkEnabled\":" + AnsiString(IniFileSettings::Instance()->GetUpdateCheckEnabled() ? "true" : "false");
      {
         // How this server is updated, asked now rather than kept in the
         // snapshot: the environment it was started in decides, and a client
         // that knows none of these four keys reads the rest as before.
         const InstallKind kind = DetectInstallKind();
         String command, advice;
         UpdatedElsewhere(kind, snapshot.available_version, command, advice);
         const char *kindName = InstallKindName(kind);
         body += ",\"installKind\":\"" + AnsiString(*kindName ? kindName : "none") + "\"";
         body += ",\"liveUpdate\":" + AnsiString(LiveUpdateApplies(kind) ? "true" : "false");
         body += ",\"updateCommand\":" + Quote_(command);
         body += ",\"updateAdvice\":" + Quote_(advice);

         // Who installs a release here: the service, an administrator at the
         // server, or whatever installed this one. A client that knows none of
         // these keys - the Deck before 6.3.6 - reads the rest as before.
         const bool administrator = UpdateInstaller::NeedsAdministrator();
         const char *installedBy = administrator ? "administrator" : (!advice.IsEmpty() || !LiveUpdateApplies(kind)) ? "elsewhere" : "service";
         body += ",\"installedBy\":\"" + AnsiString(installedBy) + "\"";
         body += ",\"installAdvice\":" + Quote_(administrator ? UpdateInstaller::InstallAdvice() : advice);
         body += ",\"serviceAccount\":" + Quote_(ServiceAccount::ProcessAccountName());
         body += ",\"serverName\":" + Quote_(UpdateInstaller::ServerName());
      }
      body += ",\"availableVersion\":" + Quote_(snapshot.available_version);
      body += ",\"releaseName\":" + Quote_(snapshot.release_name);
      body += ",\"publishedAt\":" + Quote_(snapshot.published_at);
      body += ",\"releaseUrl\":" + Quote_(snapshot.release_url);
      body += ",\"installer\":{";
      body += "\"name\":" + Quote_(snapshot.installer_name);
      body += ",\"url\":" + Quote_(snapshot.installer_url);
      body += ",\"size\":" + size;
      body += ",\"digest\":" + Quote_(snapshot.installer_digest);
      body += ",\"bundleUrl\":" + Quote_(snapshot.bundle_url);
      body += "}";
      body += ",\"downloaded\":{";
      body += "\"path\":" + Quote_(snapshot.installer_path);
      body += ",\"signer\":" + Quote_(String(snapshot.signer_identity));
      body += ",\"logTime\":" + Quote_(FormatUnixTime(snapshot.integrated_time));
      body += ",\"verifiedAt\":" + Quote_(snapshot.verified_at);
      body += "}";
      body += ",\"apply\":{";
      body += "\"status\":" + Quote_(snapshot.apply_status);
      body += ",\"version\":" + Quote_(snapshot.apply_version);
      body += ",\"detail\":" + Quote_(snapshot.apply_detail);
      body += "}";
      {
         String windowText, windowError;
         bool open = false;
         WindowStatus(windowText, open, windowError);
         body += ",\"window\":{";
         body += "\"text\":" + Quote_(windowText);
         body += ",\"open\":" + AnsiString(open ? "true" : "false");
         body += ",\"error\":" + Quote_(windowError);
         body += "}";
         body += ",\"autoDownload\":" + AnsiString(IniFileSettings::Instance()->GetUpdateAutoDownload() ? "true" : "false");
         body += ",\"backupBeforeApply\":" + AnsiString(IniFileSettings::Instance()->GetUpdateBackupBeforeApply() ? "true" : "false");
      }
      body += ",\"lastChecked\":" + Quote_(snapshot.last_checked);
      // When the scheduled check is next due, ISO 8601 UTC - it reads the feed at the
      // task's first quarter-hourly run from then: empty while it is off, and until
      // its first run since the service started.
      body += ",\"nextCheck\":" + Quote_(IniFileSettings::Instance()->GetUpdateCheckEnabled() ? FormatUnixTime(NextCheckUnix(snapshot)) : String());
      body += ",\"lastError\":" + Quote_(snapshot.last_error);
      // The document UpdateInstaller wrote, as it is, while an administrator's
      // install is prepared or ready; null otherwise.
      const bool preparing = snapshot.state == StatePreparing || snapshot.state == StateReadyForAdministrator ||
                             (snapshot.state == StateFailed && !snapshot.preparation.IsEmpty());
      body += ",\"preparation\":" + (preparing && !snapshot.preparation.IsEmpty() ? AnsiString(snapshot.preparation) : AnsiString("null"));
      body += "}";
      return body;
   }

   void
   UpdateCheckerTester::Test()
   {
      // The value of html_url as it stands in the feed's JSON (present false:
      // the release has none), and what ReadRelease_ keeps of it ("": nothing). The
      // release carries an author with an html_url of its own, as GitHub's shape
      // does, which is never the release's page.
      struct Case
      {
         std::string json;
         bool present;
         std::string kept;
         const char *what;
      };

      const std::string longest = "https://example.invalid/" + std::string(2048 - 24, 'a');
      const Case cases[] =
      {
         { "\"https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.9\"", true, "https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.9", "the project's release page" },
         { "\"HTTPS://Example.invalid/releases/6.3.9\"", true, "HTTPS://Example.invalid/releases/6.3.9", "https in capitals" },
         { "\"https://a\"", true, "https://a", "the shortest https address with a host" },
         { "\"" + longest + "\"", true, longest, "an https address of 2,048 characters" },
         { "\"" + longest + "a\"", true, "", "an https address of 2,049 characters" },
         { "\"http://example.invalid/releases/6.3.9\"", true, "", "plain http" },
         { "\"javascript:void(0)\"", true, "", "a script address" },
         { "\"JavaScript:void(0)//https://example.invalid/\"", true, "", "a script address naming an https one" },
         { "\"data:text/html,notes\"", true, "", "a data address" },
         { "\"file:///C:/notes.txt\"", true, "", "a file address" },
         { "\" https://example.invalid/\"", true, "", "a leading space" },
         { "\"\\thttps://example.invalid/\"", true, "", "a leading tab" },
         { "\"https://example.invalid/a b\"", true, "", "a space inside" },
         { "\"https://example.invalid/\\r\\nnotes\"", true, "", "a line break inside" },
         { "\"https://example.invalid/\\u0000\"", true, "", "a NUL inside" },
         { "\"https://example.invalid/\\u007f\"", true, "", "a DEL inside" },
         { "\"https://\"", true, "", "the scheme alone" },
         { "\"https:///releases/6.3.9\"", true, "", "no host" },
         { "\"https://\\\\example.invalid/\"", true, "", "a backslash for a host" },
         { "\"https://@example.invalid/\"", true, "", "an empty user for a host" },
         { "\"\"", true, "", "an empty value" },
         { "42", true, "", "a number" },
         { "", false, "", "no html_url, with the author's beside it" },
      };

      for (const Case &item : cases)
      {
         const std::string document = "{\"tag_name\":\"v6.3.9\",\"name\":\"hMailServer 6.3.9\","
            "\"author\":{\"login\":\"someone\",\"html_url\":\"https://gitlab.com/someone\"},"
            + (item.present ? "\"html_url\":" + item.json + "," : std::string()) +
            "\"published_at\":\"2026-09-30T12:00:00Z\",\"assets\":[]}";

         JsonValue value;
         std::string parseError;
         if (!JsonValue::Parse(document, value, parseError))
            throw std::logic_error(std::string("UpdateCheckerTester: the document for ") + item.what + " is not JSON: " + parseError);

         UpdateChecker::Release release;
         if (!UpdateChecker::ReadRelease_(value, release) || !release.found || release.version != FromUtf8_("6.3.9"))
            throw std::logic_error(std::string("UpdateCheckerTester: the release with ") + item.what + " as its html_url was not read; a page it cannot keep must not cost the release");

         if (release.url != FromUtf8_(item.kept))
         {
            throw std::logic_error(std::string("UpdateCheckerTester: ") + item.what + " as html_url: " +
               (item.kept.empty() ? "kept, and only an https:// address with a host and no space or control character may be" : "dropped, and it is a release page"));
         }
      }
   }
}
