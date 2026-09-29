// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SigstoreVerifier.h"
#include "ProtectedFile.h"

namespace HM
{
   // The second of the four parts of the roadmap's live update: the verify.
   //
   // Fetches the installer the last check found, and its Sigstore bundle, into the
   // data directory's Updates folder, and keeps the installer only when the bundle
   // proves it is the one this repository's release workflow signed
   // (SigstoreVerifier). A file that fails is deleted and the failure logged; it is
   // never left where anything could run it. Nothing runs here either way - that
   // is the third part, gated on this one.
   class UpdateDownloader
   {
   public:
      // On the calling thread. True when the installer is in place and verified -
      // one an earlier download left there, verified again (VerifyKept), or one
      // fetched now; false with error and the files gone. The failure is recorded as the
      // verdict (StateFailed), except where the state is not the download's to
      // change - a check answered while it ran, or an update is being installed -
      // when last_error alone says why. In a snap, a container or a build without
      // the live update, nothing is fetched: false at once, error names what
      // updates this installation, and last_error says so with the state kept.
      static bool DownloadAndVerify(String &error);

      // <DataFolder>\Updates
      static String UpdatesDirectory();

      // The Updates folder and its rollback folder, held (HeldDirectory) for as
      // long as this lives.
      struct UpdatesFolders
      {
         HeldDirectory updates;
         HeldDirectory rollback;
      };

      // Updates and Updates\rollback, made - or made again, and then emptied - so
      // that only SYSTEM, the Administrators and the service can change what is in
      // them (ProtectedFile::EnsureDirectory), and held in folders from before
      // they are checked: the installer, the rollback image and the helper wait
      // there between the server's check and their run, and neither folder, nor
      // one above them, can be renamed or swapped while folders lives. False with
      // error, reported as HM6894, when either cannot be; nothing is downloaded or
      // applied then. A repair is written to the application log.
      static bool PrepareUpdatesDirectory(UpdatesFolders &folders, String &error);

      // Fetches one installer and its bundle to installerPath (the bundle beside it as
      // .cosign.bundle) and verifies it; false with why, and nothing left, when it
      // does not verify. expectedSize and expectedDigest are the feed's hints, checked
      // when given; expectedVersion is the release the file is fetched as, which a
      // certificate naming a release tag must name (SigstoreVerifier::VerifyFile).
      // What DownloadAndVerify and the rollback image both do. Not compiled in a
      // build without the live update (HM_LIVE_UPDATE=0, UpdateChecker.h), which
      // fetches no release file.
      static bool FetchVerified(const String &installerUrl, const String &bundleUrl, const String &installerPath, __int64 expectedSize,
                                const String &expectedDigest, const String &expectedVersion, const SigstoreTrust &trust,
                                SigstoreVerdict &verdict, String &why);

      // An installer an earlier download left at installerPath, with its bundle
      // beside it, held to everything FetchVerified holds a fresh one to. True when
      // it passes. False with why empty when there is no such pair; false with why
      // saying what failed when there is one that no longer passes, which is then
      // deleted, bundle and all. What DownloadAndVerify does before it fetches, so
      // that a restart - which starts the verdict afresh - does not fetch the same
      // installer again. Not compiled without the live update, like FetchVerified.
      static bool VerifyKept(const String &installerPath, __int64 expectedSize, const String &expectedDigest, const String &expectedVersion,
                             const SigstoreTrust &trust, SigstoreVerdict &verdict, String &why);

      // Authenticode, when UpdateRequireAuthenticode=1: WinVerifyTrust on the file.
      static bool AuthenticodeTrusted(const String &path, String &why);

      // On Windows, the installer's own version resource (its fixed file version,
      // which Inno Setup writes from VersionInfoVersion) names expectedVersion;
      // false with why when it names another release or has none, or when
      // expectedVersion is empty. After the
      // download, at the apply and for the rollback image, so that a genuine old
      // installer offered as a newer release is refused even where the signer's
      // identity names no release. On Linux, true: the package's own version is
      // the update helper's to check.
      static bool InstallerVersionIs(const String &path, const String &expectedVersion, String &why);
   };
}
