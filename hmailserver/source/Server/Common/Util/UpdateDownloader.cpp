// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "UpdateDownloader.h"
#include "UpdateChecker.h"
#include "SigstoreVerifier.h"
#include "HttpsClient.h"
#include "FileUtilities.h"
#include "ProtectedFile.h"
#include "Unicode.h"
#include "../Application/IniFileSettings.h"
#include "../Application/ErrorManager.h"

// Authenticode is a Windows code-signing scheme, and WinVerifyTrust is the only
// implementation of it. See AuthenticodeTrusted below for what the POSIX build
// does instead; everything else in this file - the download, the digest and the
// Sigstore verification - is platform-neutral.
#ifdef _MSC_VER
#include <wintrust.h>
#include <softpub.h>
#include <winver.h>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "version.lib")
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
#if HM_LIVE_UPDATE
   namespace
   {
      // An installer is seventy-odd megabytes; a bundle, eleven kilobytes.
      const size_t MAX_INSTALLER_BYTES = 1024u * 1024u * 1024u;
      const size_t MAX_BUNDLE_BYTES = 1024u * 1024u;

      void Remove_(const String &path)
      {
         if (!path.IsEmpty() && FileUtilities::Exists(path))
            FileUtilities::DeleteFile(path);
      }
   }
#endif

   String
   UpdateDownloader::UpdatesDirectory()
   {
      String directory = IniFileSettings::Instance()->GetDataDirectory();
      if (directory.IsEmpty())
         return _T("");
      if (directory.Right(1) != FileUtilities::PathSeparator)
         directory += FileUtilities::PathSeparator;
      return directory + _T("Updates");
   }

   bool
   UpdateDownloader::PrepareUpdatesDirectory(UpdatesFolders &folders, String &error)
   {
      folders.updates.Release();
      folders.rollback.Release();

      String directory = UpdatesDirectory();
      if (directory.IsEmpty())
      {
         error = _T("The data directory is not configured.");
         return false;
      }

      // Updates first, and held, so that rollback is made - or checked - inside
      // the folder that was.
      const String paths[2] = { directory, directory + FileUtilities::PathSeparator + _T("rollback") };
      HeldDirectory *held[2] = { &folders.updates, &folders.rollback };
      for (int i = 0; i < 2; i++)
      {
         String repaired;
         String why;
         if (!ProtectedFile::EnsureDirectory(paths[i], *held[i], repaired, why))
         {
            folders.updates.Release();
            folders.rollback.Release();
            error = Formatter::Format(_T("The update was refused: {0}"), why);
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6894, "UpdateDownloader::PrepareUpdatesDirectory", error);
            return false;
         }
         // Braces: LOG_APPLICATION is itself an if statement.
         if (!repaired.IsEmpty())
         {
            LOG_APPLICATION(Formatter::Format(_T("Update: the permissions of {0} were set to SYSTEM, the Administrators and the service alone; before, {1}."),
               paths[i], repaired));
         }
      }
      return true;
   }

   bool
   UpdateDownloader::AuthenticodeTrusted(const String &path, String &why)
   {
#ifndef _MSC_VER
      // There is no Authenticode outside Windows, so there is nothing here to ask.
      // Refusing is the only safe answer: an administrator who set
      // UpdateRequireAuthenticode=1 asked for the signature to be checked, and a
      // build that cannot check it must not report that it passed. The roadmap
      // section "Linux and AArch64" leaves updating a Linux install to its package
      // manager rather than to a signed Windows installer.
      why = _T("Authenticode signatures can only be checked on Windows");
      return false;
#else
      WINTRUST_FILE_INFO fileInfo;
      memset(&fileInfo, 0, sizeof(fileInfo));
      fileInfo.cbStruct = sizeof(fileInfo);
      fileInfo.pcwszFilePath = path.c_str();

      WINTRUST_DATA data;
      memset(&data, 0, sizeof(data));
      data.cbStruct = sizeof(data);
      data.dwUIChoice = WTD_UI_NONE;
      data.fdwRevocationChecks = WTD_REVOKE_NONE;
      data.dwUnionChoice = WTD_CHOICE_FILE;
      data.pFile = &fileInfo;
      data.dwStateAction = WTD_STATEACTION_VERIFY;
      data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

      GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
      LONG status = WinVerifyTrust((HWND) INVALID_HANDLE_VALUE, &policy, &data);

      data.dwStateAction = WTD_STATEACTION_CLOSE;
      WinVerifyTrust((HWND) INVALID_HANDLE_VALUE, &policy, &data);

      if (status == ERROR_SUCCESS)
         return true;

      switch (status)
      {
      case TRUST_E_NOSIGNATURE:
         why = _T("the file carries no Authenticode signature");
         break;
      case TRUST_E_SUBJECT_FORM_UNKNOWN:
         why = _T("the file is not a signed Windows binary");
         break;
      case TRUST_E_EXPLICIT_DISTRUST:
         why = _T("the signer is explicitly distrusted on this machine");
         break;
      case TRUST_E_SUBJECT_NOT_TRUSTED:
         why = _T("the signer is not trusted on this machine");
         break;
      case CERT_E_UNTRUSTEDROOT:
         why = _T("the signing certificate chains to a root this machine does not trust");
         break;
      case TRUST_E_BAD_DIGEST:
         why = _T("the file has been modified since it was signed");
         break;
      default:
         why.Format(_T("WinVerifyTrust answered 0x%08X"), (unsigned) status);
         break;
      }
      return false;
#endif
   }

   bool
   UpdateDownloader::InstallerVersionIs(const String &path, const String &expectedVersion, String &why)
   {
#ifndef _MSC_VER
      // A package names its version in the package, and the Linux update helper
      // reads it there and installs nothing that is not an upgrade of what is
      // installed (hmailserver-update). There is no version resource to read.
      (void) path;
      (void) expectedVersion;
      why.Empty();
      return true;
#else
      // No release to hold the file to is no reason to let it through: every
      // caller names one, and an empty one means the snapshot lost it.
      if (expectedVersion.IsEmpty())
      {
         why = _T("There is no release to hold the installer to, so nothing shows it is the one to install.");
         return false;
      }

      // The installer's own word on what it installs: the fixed file version Inno
      // Setup writes from VersionInfoVersion (<version>.0, section_setup_64.iss),
      // which every installer since 6.2.19 carries. The Sigstore bundle has
      // already bound these bytes to the signer, so the resource is the signer's
      // own statement; where UpdateRequireAuthenticode is on, the Authenticode
      // signature covers it as well. A file that says another release is not the
      // release the feed offered it as, whatever the feed calls it - the check a
      // signer whose identity names no release (the GitHub workflow, run from a
      // branch) cannot make itself.
      DWORD ignored = 0;
      const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
      if (size == 0)
      {
         why = Formatter::Format(_T("The installer carries no version resource, so nothing in it says it is release {0}."), expectedVersion);
         return false;
      }
      std::vector<unsigned char> data(size);
      VS_FIXEDFILEINFO *info = nullptr;
      UINT length = 0;
      if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
          !VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID *>(&info), &length) ||
          info == nullptr || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
      {
         why = Formatter::Format(_T("The installer's version resource cannot be read, so nothing in it says it is release {0}."), expectedVersion);
         return false;
      }

      String version;
      version.Format(_T("%u.%u.%u.%u"), (unsigned) HIWORD(info->dwFileVersionMS), (unsigned) LOWORD(info->dwFileVersionMS),
         (unsigned) HIWORD(info->dwFileVersionLS), (unsigned) LOWORD(info->dwFileVersionLS));
      if (UpdateChecker::CompareVersions(version, expectedVersion) != 0)
      {
         why = Formatter::Format(_T("The installer says it is version {0}, not {1}, the release it was offered as."), version, expectedVersion);
         return false;
      }
      return true;
#endif
   }

   // The fetch of a release file, for the download below and the apply step's
   // rollback image. A build without the live update (HM_LIVE_UPDATE=0,
   // UpdateChecker.h) fetches no release file, and compiles neither.
#if HM_LIVE_UPDATE
   namespace
   {
      // What an installer has to pass before it is kept, and a kept one before it
      // is used again: the feed's size and digest when it states them, the Sigstore
      // bundle beside it, its own version resource, and Authenticode where
      // UpdateRequireAuthenticode asks for it. False with why naming the first that
      // failed; described names the file in why ("the download", "the kept copy").
      bool CheckInstaller_(const String &path, const String &bundlePath, __int64 expectedSize, const String &expectedDigest,
                           const String &expectedVersion, const SigstoreTrust &trust, const TCHAR *described,
                           SigstoreVerdict &verdict, String &why)
      {
         unsigned __int64 size = 0;
         FileUtilities::FileSize64(path, size);
         if (expectedSize > 0 && (__int64) size != expectedSize)
         {
            why = Formatter::Format(_T("The feed says the installer is {0} bytes; {1} is {2}."), expectedSize, String(described), (__int64) size);
            return false;
         }

         // The feed's digest is a cheap consistency check, not the proof; the
         // proof is the bundle. A disagreement between the two means one of them
         // is not what it claims, and that is enough to stop.
         std::vector<unsigned char> digest;
         String hashError;
         if (!SigstoreVerifier::Sha256File(path, digest, hashError))
         {
            why = hashError;
            return false;
         }
         AnsiString stated = AnsiString(expectedDigest);
         stated.MakeLower();
         if (stated.StartsWith("sha256:") && std::string(stated.c_str() + 7) != SigstoreVerifier::Hex(digest))
         {
            why = Formatter::Format(_T("The feed says the installer's SHA-256 is {0}; {1}'s is {2}."),
               String(stated.c_str() + 7), String(described), String(SigstoreVerifier::Hex(digest).c_str()));
            return false;
         }

         AnsiString bundleJson;
         SigstoreVerifier::ReadBundle(bundlePath, bundleJson);
         String versionWhy;
         if (!SigstoreVerifier::VerifyFile(path, bundleJson, trust, verdict, expectedVersion))
            why = Formatter::Format(_T("The installer did not verify against its Sigstore bundle: {0}"), verdict.error);
         else if (!UpdateDownloader::InstallerVersionIs(path, expectedVersion, versionWhy))
            why = Formatter::Format(_T("The installer is not the release it was offered as: {0}"), versionWhy);
         else if (IniFileSettings::Instance()->GetUpdateRequireAuthenticode() && !UpdateDownloader::AuthenticodeTrusted(path, why))
            why = Formatter::Format(_T("The installer's Authenticode signature was refused: {0}."), why);
         else
            return true;
         return false;
      }
   }

   bool
   UpdateDownloader::FetchVerified(const String &installerUrl, const String &bundleUrl, const String &installerPath, __int64 expectedSize,
                                   const String &expectedDigest, const String &expectedVersion, const SigstoreTrust &trust,
                                   SigstoreVerdict &verdict, String &why)
   {
      String bundlePath = installerPath + _T(".cosign.bundle");
      String partialPath = installerPath + _T(".partial");
      bool ok = false;
      int status = 0;

      // The bundle first: it is small, and without it the installer is not worth
      // the bandwidth.
      if (!HttpsClient::Download(AnsiString(bundleUrl), bundlePath, MAX_BUNDLE_BYTES, status, why))
         why = Formatter::Format(_T("The Sigstore bundle could not be fetched: {0}"), why);
      else if (!HttpsClient::Download(AnsiString(installerUrl), partialPath, MAX_INSTALLER_BYTES, status, why))
         why = Formatter::Format(_T("The installer could not be fetched: {0}"), why);
      else if (CheckInstaller_(partialPath, bundlePath, expectedSize, expectedDigest, expectedVersion, trust, _T("the download"), verdict, why))
      {
         Remove_(installerPath);
         if (!FileUtilities::Move(partialPath, installerPath))
            why = Formatter::Format(_T("{0} could not be moved into place."), partialPath);
         else
            ok = true;
      }

      if (!ok)
      {
         Remove_(partialPath);
         Remove_(bundlePath);
      }
      return ok;
   }

   bool
   UpdateDownloader::VerifyKept(const String &installerPath, __int64 expectedSize, const String &expectedDigest, const String &expectedVersion,
                                const SigstoreTrust &trust, SigstoreVerdict &verdict, String &why)
   {
      why.Empty();
      String bundlePath = installerPath + _T(".cosign.bundle");
      if (!FileUtilities::Exists(installerPath) || !FileUtilities::Exists(bundlePath))
         return false;

      if (CheckInstaller_(installerPath, bundlePath, expectedSize, expectedDigest, expectedVersion, trust, _T("the kept copy"), verdict, why))
         return true;

      // Not the file it was: gone, both of it, and fetched again by the caller.
      Remove_(installerPath);
      Remove_(bundlePath);
      return false;
   }
#endif

   bool
   UpdateDownloader::DownloadAndVerify(String &error)
   {
      error.Empty();

      UpdateChecker::Snapshot snapshot = UpdateChecker::Current();

      // A snap, a container or a build without the live update is updated by
      // what installed it: nothing is fetched, and the reason names how it is
      // updated. The state stays what the check said - a newer release is
      // available - and last_error says why this server does not fetch it.
      {
         String command;
         if (UpdateChecker::UpdatedElsewhere(UpdateChecker::DetectInstallKind(), snapshot.available_version, command, error))
         {
            UpdateChecker::RecordLastError(error);
            LOG_APPLICATION(Formatter::Format(_T("Update download refused: {0}"), error));
            return false;
         }
      }

#if !HM_LIVE_UPDATE
      // Not reached: every kind a build without the live update detects is one
      // UpdatedElsewhere answers for.
      error = _T("This build of hMailServer was built without the live update, so it downloads no release file.");
      return false;
#else
      // The helper may be about to run the waiting installer, or running it: a
      // download now would put another file where it is.
      if (snapshot.state == UpdateChecker::StateInstalling)
      {
         error = Formatter::Format(_T("hMailServer {0} is being installed; nothing is downloaded until its outcome is known."), snapshot.available_version);
         UpdateChecker::RecordLastError(error);
         return false;
      }
      if (snapshot.available_version.IsEmpty())
      {
         error = _T("No newer release is known; run a check first.");
         return false;
      }
      if (snapshot.installer_url.IsEmpty() || snapshot.installer_name.IsEmpty())
      {
         error = Formatter::Format(_T("Release {0} carries no {1} to download."), snapshot.available_version, UpdateChecker::MissingArtefactDescription(snapshot.available_version));
         return false;
      }
      if (snapshot.bundle_url.IsEmpty())
      {
         error = Formatter::Format(_T("Release {0} carries no Sigstore bundle for its installer, so it cannot be verified."), snapshot.available_version);
         return false;
      }

      // Held until this returns: the download is written into the folder that was
      // checked, and the folder cannot be renamed or swapped meanwhile.
      String directory = UpdatesDirectory();
      UpdatesFolders folders;
      if (!PrepareUpdatesDirectory(folders, error))
      {
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update download refused: {0}"), error));
         return false;
      }

      SigstoreTrust trust;
      String trustError;
      if (!SigstoreVerifier::ConfiguredTrust(trust, trustError))
      {
         UpdateChecker::RecordFailure(trustError);
         LOG_APPLICATION(Formatter::Format(_T("Update download refused: {0}"), trustError));
         error = trustError;
         return false;
      }

      // The name came from the feed. UpdateChecker refuses a tag that is not a
      // version number, so this cannot normally be anything but a file name - and
      // this is the line that would be exploited if that ever stopped being true,
      // so it does not take the checker's word for it.
      if (snapshot.installer_name.Find(_T("\\")) >= 0 || snapshot.installer_name.Find(_T("/")) >= 0 ||
          snapshot.installer_name.Find(_T(":")) >= 0 || snapshot.installer_name.Find(_T("..")) >= 0 ||
          snapshot.installer_name.IsEmpty())
      {
         error = _T("The release names its installer with a file name this server will not write. Nothing was downloaded.");
         UpdateChecker::RecordFailure(error);
         LOG_APPLICATION(Formatter::Format(_T("Update download refused: {0}"), error));
         return false;
      }

      String installerPath = directory + FileUtilities::PathSeparator + snapshot.installer_name;
      SigstoreVerdict verdict;
      String why;

      // The installer an earlier download verified, still here - kept across a
      // restart, since the verdict lives in the process and the file on disk: it is
      // verified again where it is, as a fresh download would be, and fetched only
      // when it no longer passes. 6.3.4 and 6.3.5 fetched it again after every
      // restart, and a service that cannot install it (NT SERVICE\hMailServer,
      // docs/ServiceAccount.md) with UpdateAutoDownload=1 downloaded the whole
      // installer each time it started.
      String keptWhy;
      if (VerifyKept(installerPath, snapshot.installer_size, snapshot.installer_digest, snapshot.available_version, trust, verdict, keptWhy))
      {
         if (!UpdateChecker::RecordDownloaded(installerPath, snapshot.available_version, verdict.identity, verdict.integrated_time))
         {
            // A check answered meanwhile and offers another release; the file is
            // this one's, and the sweep takes it once it is not on offer.
            error = Formatter::Format(_T("A check while hMailServer {0} was being verified changed the release on offer. Check again, then download again."),
               snapshot.available_version);
            UpdateChecker::RecordLastError(error);
            return false;
         }
         LOG_APPLICATION(Formatter::Format(_T("Update: hMailServer {0} is already in {1} and verified again: signed by {2}, recorded by the transparency log at {3}. Nothing was fetched."),
            snapshot.available_version, installerPath, String(verdict.identity), UpdateChecker::FormatUnixTime(verdict.integrated_time)));
         return true;
      }
      if (!keptWhy.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format(_T("Update: the copy of hMailServer {0} kept in {1} did not verify again, so it is deleted and fetched again: {2}"),
            snapshot.available_version, installerPath, keptWhy));
      }

      if (!FetchVerified(snapshot.installer_url, snapshot.bundle_url, installerPath, snapshot.installer_size, snapshot.installer_digest,
                         snapshot.available_version, trust, verdict, why))
      {
         UpdateChecker::RecordFailure(why);
         LOG_APPLICATION(Formatter::Format(_T("Update download failed for hMailServer {0}: {1}"), snapshot.available_version, why));
         error = why;
         return false;
      }

      if (!UpdateChecker::RecordDownloaded(installerPath, snapshot.available_version, verdict.identity, verdict.integrated_time))
      {
         // A check answered while the file was coming down, and the release it
         // offers is no longer this one. The file is verified - as this release -
         // and is not kept as the one waiting to be installed.
         Remove_(installerPath);
         Remove_(installerPath + _T(".cosign.bundle"));
         error = Formatter::Format(_T("A check while hMailServer {0} was downloading changed the release on offer; the download is not kept. Check again, then download again."),
            snapshot.available_version);
         // The state is the check's, and stays so; the reason is the download's.
         UpdateChecker::RecordLastError(error);
         LOG_APPLICATION(Formatter::Format(_T("Update download not kept: {0}"), error));
         return false;
      }
      LOG_APPLICATION(Formatter::Format(_T("Update: hMailServer {0} downloaded to {1} and verified: signed by {2}, recorded by the transparency log at {3}."),
         snapshot.available_version, installerPath, String(verdict.identity), UpdateChecker::FormatUnixTime(verdict.integrated_time)));
      return true;
#endif
   }
}
