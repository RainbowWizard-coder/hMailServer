// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Whether this build carries the live update's apply step. Every Windows build
// does. A POSIX build does unless it was configured with -DHM_LIVE_UPDATE=OFF
// (hmailserver/source/Server/CMakeLists.txt), which is how a distribution, a
// store or anyone whose package manager updates the server builds it: the check
// still says that a release exists, and names HM_UPDATE_COMMAND - the command
// the distribution gives its users, "apt upgrade hmailserver" - in place of
// offering a file, and nothing downloads or installs one (UpdateChecker::
// UpdatedElsewhere). CMake defines both for every POSIX build; the defaults
// below are for the Windows project and for the scripts that compile one core
// file at a time with -DHM_PLATFORM_POSIX=1 alone.
#ifndef HM_LIVE_UPDATE
#define HM_LIVE_UPDATE 1
#endif
#ifndef HM_UPDATE_COMMAND
#define HM_UPDATE_COMMAND ""
#endif
#if !HM_LIVE_UPDATE && !defined(HM_PLATFORM_POSIX)
#error "HM_LIVE_UPDATE=0 is an option of the POSIX build; the Windows server always carries the live update."
#endif

namespace HM
{
   class JsonValue;

   // Does the project have a newer release than the one running.
   //
   // The first of the four parts of the roadmap's live update: the check. The feed
   // is the project's own, https://updates.progressiverobot.com, static files in
   // the GitHub Releases API's shape - releases/latest on the stable channel, the
   // newest release that is not a pre-release, or releases.json, the list, on the
   // pre-release channel - unless UpdateFeedUrl names another, which is how the
   // tests point it at a fake, how a site without Internet access could point it at
   // a mirror, and how a server from 6.2.28 to 6.3.3, built to read the GitHub
   // Releases API, is pointed at this one (docs/UpdateFeed.md). Nothing but the
   // request goes out: no identifier, no configuration, just a GET with the
   // User-Agent the client always sends.
   //
   // The verdict is kept here, process-wide, and read by the Status COM object, the
   // REST route /api/v1/update and the Control Panel; every check writes one
   // application-log line so the history is in the log where the rest of the
   // server's is. Nothing is downloaded and nothing runs: that is the second and
   // third part, and each is gated on this one having found something.
   class UpdateChecker
   {
   public:
      enum State
      {
         StateNotChecked = 0,     // no check has completed since the service started
         StateUpToDate = 1,       // the running version is the latest on the channel
         StateAvailable = 2,      // a newer release exists; its details are below
         StateDownloaded = 3,     // its installer is in the data directory and verified
         StateInstalling = 4,     // the installer has been handed to the helper
         StateFailed = 5          // the last check failed; last_error says why
      };

      struct Snapshot
      {
         Snapshot() : state(StateNotChecked), installer_size(0), integrated_time(0), installing_since_unix(0), last_check_unix(0) {}

         State state;
         String available_version;     // "6.2.28"; empty unless a newer release is known
         String release_name;          // the release's title
         String published_at;          // as the feed gives it: ISO 8601, UTC
         String release_url;           // the release page
         String installer_name;        // hMailServer-<version>-x64.exe, when the release has it
         String installer_url;
         __int64 installer_size;
         String installer_digest;      // "sha256:<hex>" as the feed states it; a hint, not the proof
         String bundle_url;            // the installer's Sigstore bundle
         String installer_path;        // where the verified installer is, from StateDownloaded on
         AnsiString signer_identity;   // the workflow identity the bundle's certificate names
         __int64 integrated_time;      // when the transparency log recorded the signature, Unix seconds
         String verified_at;           // local time the installer was verified
         String apply_status;          // the helper's last outcome: ok, failed, rolled-back, rollback-failed, no-rollback
         String apply_version;         // the version that outcome was about
         String apply_detail;          // what the helper said
         __int64 installing_since_unix; // when the installer was handed over, Unix seconds; 0 unless StateInstalling
         String last_checked;          // local time; empty until a check has completed
         __int64 last_check_unix;      // the same as Unix seconds; 0 until then
         String last_error;            // empty unless the last check failed
      };

      static Snapshot Current();

      // How this server was installed, which decides which of a release's artefacts
      // it could install and how the apply step goes about it. On Windows always
      // the installer. On Linux, in this order:
      //   - a container when the image says so (HM_INSTALL_KIND=container, which
      //     the Dockerfile sets): the image installs the .deb, and without the
      //     mark the server took itself for a .deb installation and offered to
      //     replace the package inside a container that is replaced, not updated;
      //   - a snap when snapd started it (SNAP in the environment, which snapd
      //     sets for every snap's processes, naming the directory this program's
      //     own file is in - not a SNAP inherited from a snap's terminal);
      //   - in a build without the live update (HM_LIVE_UPDATE=0), the
      //     distribution's own package, whatever package manager recorded it;
      //   - the AppImage when the AppImage runtime says so (APPIMAGE in the
      //     environment, set by the runtime before AppRun starts the server), the
      //     .deb when dpkg's record says it owns this package, the .rpm when rpm's
      //     does, and nothing for a build tree or a tarball, which no package
      //     could replace.
      // The last three kinds are appended so that the first five keep their
      // values.
      enum InstallKind
      {
         InstallWindowsInstaller = 0,
         InstallDeb,
         InstallRpm,
         InstallAppImage,
         InstallNone,
         InstallSnap,
         InstallContainer,
         InstallDistribution
      };
      static InstallKind DetectInstallKind();
      // The word the apply request carries: "deb", "rpm", "appimage"; "installer"
      // on Windows, "" for none; and "snap", "container" and "distribution" for
      // the kinds no request is ever written for.
      static const char *InstallKindName(InstallKind kind);

      // Whether a release file can update this installation in place: a build
      // with the apply step (HM_LIVE_UPDATE), and a kind the apply step knows -
      // the installer, the .deb, the .rpm or the AppImage.
      static bool LiveUpdateApplies(InstallKind kind);

      // For an installation something other than this server updates - a snap
      // (snapd), a container (its image), a distribution's package (its package
      // manager) - true, with the command that updates it and the sentence the
      // check, the download and the apply say: the command may be empty, for a
      // build without the live update that was given no HM_UPDATE_COMMAND. The
      // version is the release on offer, which the container's image tag names;
      // empty, the tag is latest. False, and both empty, for every other kind,
      // including none, whose refusals say what they always said.
      static bool UpdatedElsewhere(InstallKind kind, const String &version, String &command, String &sentence);

      // The artefact this server could install, by the name the release flow gives
      // it - hMailServer-<v>-x64.exe, hmailserver_<v>_amd64.deb,
      // hmailserver-<v>-1.x86_64.rpm, hMailServer-<v>-x86_64.AppImage, or their
      // AArch64 forms - or empty when there is none for this installation.
      static String PlatformInstallerName(const String &version);

      // What a message calls that artefact when a release lacks it: "x64 installer"
      // on Windows (a wording a test pins), or the name itself here.
      static String MissingArtefactDescription(const String &version);

      // Reads the feed on the calling thread. True when it was read and understood;
      // the snapshot then says what it said. False, with error set and the snapshot
      // recording the failure, when it was not. Either way one application-log line.
      static bool CheckNow(String &error);

      // The feed's URL: UpdateFeedUrl, or the project's feed for the channel.
      static AnsiString FeedUrl();
      static bool IsPreReleaseChannel();

      // The version this binary is, "6.2.28": HMAILSERVER_VERSION without the build.
      static String RunningVersion();

      // Numeric, component by component: "6.10.0" is newer than "6.2.28", a leading
      // "v" and anything from the first character that is neither a digit nor a dot
      // ("-B37") are ignored, and missing components are zero. <0, 0 or >0.
      static int CompareVersions(const String &left, const String &right);

      static const char *StateName(State state);

      // The download's outcome, recorded by UpdateDownloader: version is the release
      // the file was downloaded and verified as. False, and nothing recorded, when
      // that is no longer the release the snapshot offers - a check answered while
      // the download ran - since a file kept as "downloaded" is installed as
      // available_version, and one verified as another release must not be.
      static bool RecordDownloaded(const String &path, const String &version, const AnsiString &identity, __int64 integratedTime);
      // A failure of a check or a download: the state becomes StateFailed and
      // last_error says why; what an earlier check learned is kept.
      static void RecordFailure(const String &reason);
      // A request refused without changing what the snapshot says: last_error says
      // why and the state stays as it is - a download a check overtook, whose
      // state is the check's, or one asked for while an update is being
      // installed, whose state is installing.
      static void RecordLastError(const String &reason);

      // The installer has been handed to the helper.
      static void RecordInstalling();
      // The helper's outcome, read at startup by UpdateInstaller::ReportLastOutcome.
      static void RecordApplyOutcome(const String &status, const String &version, const String &detail);
      // True, once, when the installer was handed over more than allowedSeconds
      // ago and this same process is still running with no outcome to show for
      // it: the helper never stopped the service, so it refused the request or
      // died before installing anything. The state becomes StateFailed with the
      // reason given, and the question is not asked again for that hand-over.
      static bool RecordInstallingTimedOut(__int64 allowedSeconds, const String &reason);

      // One release's installer and bundle, as a rollback image needs them.
      struct ReleaseAssets
      {
         ReleaseAssets() : installer_size(0) {}

         String version;
         String installer_url;
         __int64 installer_size;
         String installer_digest;
         String bundle_url;
      };

      // The release the feed publishes under tag v<version>: releases/tags/v<version>
      // beside the feed's own path, so a mirror or the tests' fake serves it too.
      static AnsiString ReleaseByTagUrl(const String &version);
      static bool FetchReleaseByTag(const String &version, ReleaseAssets &assets, String &error);

      // UpdateCheckHours since the last check, or no check yet: the scheduled task
      // runs every quarter hour and reads the feed only when this says so.
      static bool CheckIsDue();
      // Makes the next check due: called when the servers (re)start, so a changed
      // UpdateChannel or UpdateFeedUrl is read at once rather than a day later.
      static void ResetSchedule();
      // UpdateWindow as configured: its normalised text, whether it is open now,
      // and the parse error if it has one (text is then the setting as written).
      static void WindowStatus(String &text, bool &open, String &error);

      // Unix seconds as ISO 8601 UTC.
      static String FormatUnixTime(__int64 seconds);

      // The snapshot as the JSON document the REST route returns. Beside the
      // snapshot it says how this server is updated, from DetectInstallKind at
      // the time of asking: installKind (InstallKindName, "none" for none),
      // liveUpdate (LiveUpdateApplies), and updateCommand and updateAdvice
      // (UpdatedElsewhere; empty where the live update does it).
      static AnsiString ToJson(const Snapshot &snapshot);

   private:
      struct Release
      {
         Release() : found(false), draft(false), prerelease(false), installer_size(0) {}

         bool found;
         String version;
         String name;
         String url;
         String published_at;
         bool draft;
         bool prerelease;
         String installer_name;
         String installer_url;
         __int64 installer_size;
         String installer_digest;
         String bundle_url;
      };

      static bool ReadRelease_(const JsonValue &value, Release &release);
      // The newest release the channel accepts from a feed that is one release or a
      // list of them. found is false when the feed had releases but the channel took
      // none of them (a pre-release on the stable channel), which is not an error.
      static bool SelectRelease_(const JsonValue &document, bool preReleaseChannel, Release &release, String &error);
      static bool Fail_(const String &reason, String &error);
      static AnsiString Quote_(const String &value);

      static boost::mutex mutex_;
      static Snapshot snapshot_;
   };
}
