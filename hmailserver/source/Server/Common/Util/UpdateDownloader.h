// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SigstoreVerifier.h"

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
      // On the calling thread. True when the installer is in place and verified;
      // false with error and the files gone. The failure is recorded as the
      // verdict (StateFailed), except where the state is not the download's to
      // change - a check answered while it ran, or an update is being installed -
      // when last_error alone says why.
      static bool DownloadAndVerify(String &error);

      // <DataFolder>\Updates
      static String UpdatesDirectory();

      // Updates and Updates\rollback, made - or made again - so that only SYSTEM,
      // the Administrators and the service can change what is in them
      // (ProtectedFile::EnsureDirectory): the installer, the rollback image and the
      // helper wait there between the server's check and their run. False with
      // error, reported as HM6894, when either cannot be; nothing is downloaded or
      // applied then. A repair is written to the application log.
      static bool PrepareUpdatesDirectory(String &error);

      // Fetches one installer and its bundle to installerPath (the bundle beside it as
      // .cosign.bundle) and verifies it; false with why, and nothing left, when it
      // does not verify. expectedSize and expectedDigest are the feed's hints, checked
      // when given; expectedVersion is the release the file is fetched as, which a
      // certificate naming a release tag must name (SigstoreVerifier::VerifyFile).
      // What DownloadAndVerify and the rollback image both do.
      static bool FetchVerified(const String &installerUrl, const String &bundleUrl, const String &installerPath, __int64 expectedSize,
                                const String &expectedDigest, const String &expectedVersion, const SigstoreTrust &trust,
                                SigstoreVerdict &verdict, String &why);

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
