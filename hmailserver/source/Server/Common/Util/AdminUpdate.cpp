// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#ifndef HM_PLATFORM_POSIX

#include "AdminUpdate.h"
#include "AdminUpdateChecks.h"
#include "AdminUpdateTester.h"
#include "AuthenticodePublisher.h"
#include "LoweredChild.h"
#include "SigstoreVerifier.h"
#include "UpdateChecker.h"
#include "UpdateDownloader.h"
#include "UpdateInstaller.h"
#include "../Application/Version.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winternl.h>

#include <algorithm>
#include <map>

#pragma comment(lib, "bcrypt.lib")

// NOTHING IN THIS FILE LOGS. The stage runs elevated, and the log folder, the
// error log and the data folder are the service's to write: a line written there
// by an administrator's process could go wherever a link the service planted
// leads (R3 of the design, docs/ServiceAccount.md). Every line the stage writes
// goes to stage.log in its own work folder, which only administrators can write;
// Logger::ForbidWritesForThisProcess stops anything it reaches from writing
// elsewhere, and build/check-elevated-verbs.py holds this file to no LOG_ and no
// ErrorManager.

namespace HM
{
   namespace
   {
      const wchar_t *const ServiceName_ = L"hMailServer";
      const wchar_t *const InstallUpdateVerb_ = L"/InstallUpdate";
      const wchar_t *const VerifyBundleVerb_ = L"--verify-bundle";

      const unsigned long long InstallerCap_ = 1024ull * 1024 * 1024;   // 1 GB: the 6.3.5 installer is 88 MB
      const unsigned long long BundleCap_ = 1024ull * 1024;             // 1 MB: a bundle is under 12 KB
      const unsigned long long HelperCap_ = 64ull * 1024 * 1024;
      const unsigned long long FreeSpaceNeeded_ = 1024ull * 1024 * 1024;
      const unsigned long RevocationTimeoutMilliseconds_ = 30000;
      const unsigned long ChildTimeoutMilliseconds_ = 120000;
      const unsigned long PreparationWaitMilliseconds_ = 90 * 60 * 1000;
      const int WorkFoldersKept_ = 3;

      // The request frame of --verify-bundle.
      const char RequestMagic_[4] = { 'H', 'M', 'V', 'B' };
      const unsigned char RequestFormat_ = 1;
      const size_t RequestCap_ = 6 * 1024 * 1024;
      const size_t StringCap_ = 4 * 1024 * 1024;
      const uint32_t ListCap_ = 16;

      std::wstring Wide_(const std::string &utf8)
      {
         if (utf8.empty())
            return std::wstring();
         const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
         std::wstring out((size_t) (needed > 0 ? needed : 0), L'\0');
         if (needed > 0)
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), &out[0], needed);
         return out;
      }

      std::string Utf8_(const std::wstring &wide)
      {
         if (wide.empty())
            return std::string();
         const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), nullptr, 0, nullptr, nullptr);
         std::string out((size_t) (needed > 0 ? needed : 0), '\0');
         if (needed > 0)
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int) wide.size(), &out[0], needed, nullptr, nullptr);
         return out;
      }

      std::wstring Join_(const std::wstring &folder, const std::wstring &name)
      {
         return folder.empty() || folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
      }

      std::wstring DirectoryOf_(const std::wstring &path)
      {
         const size_t slash = path.find_last_of(L"\\/");
         return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
      }

      std::wstring NameOf_(const std::wstring &path)
      {
         const size_t slash = path.find_last_of(L"\\/");
         return slash == std::wstring::npos ? path : path.substr(slash + 1);
      }

      std::wstring ModulePath_()
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD) buffer.size());
         return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
      }

      // The long form of a path, 8.3 components expanded - without resolving a
      // link: GetLongPathName reads directory entries, it does not follow them.
      std::wstring LongPath_(const std::wstring &path)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), (DWORD) buffer.size());
         return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : path;
      }

      // A path with its "." and ".." taken out, as Windows would take them out
      // when it opened it - a string operation, nothing is looked up - so that it
      // can be compared with the path Windows resolves a handle to.
      std::wstring FullPath_(const std::wstring &path)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetFullPathNameW(path.c_str(), (DWORD) buffer.size(), buffer.data(), nullptr);
         return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : path;
      }

      std::wstring OneLine_(std::wstring text)
      {
         for (wchar_t &c : text)
         {
            if (c == L'\r' || c == L'\n' || c == L'\t')
               c = L' ';
         }
         return text;
      }

      std::wstring UtcNow_()
      {
         SYSTEMTIME t;
         GetSystemTime(&t);
         wchar_t text[32];
         swprintf_s(text, L"%04d-%02d-%02dT%02d:%02d:%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
         return text;
      }

      // The service's registered program, as the service manager has it.
      std::wstring ServiceBinary_()
      {
         std::wstring binary;
         SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
         if (!manager)
            return binary;
         SC_HANDLE service = OpenServiceW(manager, ServiceName_, SERVICE_QUERY_CONFIG);
         if (service)
         {
            DWORD needed = 0;
            QueryServiceConfigW(service, nullptr, 0, &needed);
            if (needed)
            {
               std::vector<BYTE> buffer(needed);
               QUERY_SERVICE_CONFIGW *config = (QUERY_SERVICE_CONFIGW *) buffer.data();
               if (QueryServiceConfigW(service, config, needed, &needed) && config->lpBinaryPathName)
               {
                  std::wstring line = config->lpBinaryPathName;
                  if (!line.empty() && line[0] == L'"')
                  {
                     const size_t close = line.find(L'"', 1);
                     binary = close == std::wstring::npos ? line.substr(1) : line.substr(1, close - 1);
                  }
                  else
                  {
                     const size_t space = line.find(L' ');
                     binary = space == std::wstring::npos ? line : line.substr(0, space);
                  }
               }
            }
            CloseServiceHandle(service);
         }
         CloseServiceHandle(manager);
         return binary;
      }

      // A privilege of this process's token switched on, or off again. Held by an
      // elevated administrator's token either way; enabled, it is in force without
      // a further step for this process and for every process it starts.
      bool SetPrivilege_(const wchar_t *name, bool enabled)
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
            return false;
         TOKEN_PRIVILEGES privileges;
         privileges.PrivilegeCount = 1;
         privileges.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
         bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
                   AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
         CloseHandle(token);
         return ok;
      }

      bool IsElevatedAdministrator_()
      {
         BYTE buffer[SECURITY_MAX_SID_SIZE];
         DWORD size = sizeof(buffer);
         BOOL member = FALSE;
         return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, buffer, &size) && CheckTokenMembership(nullptr, buffer, &member) && member;
      }

      std::wstring TokenUserOf_(HANDLE token, std::wstring &sid)
      {
         sid.clear();
         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         std::vector<unsigned char> buffer(needed ? needed : 1);
         if (!GetTokenInformation(token, TokenUser, buffer.data(), (DWORD) buffer.size(), &needed))
            return std::wstring();
         PSID user = ((TOKEN_USER *) buffer.data())->User.Sid;
         sid = AdminUpdateChecks::TextOfSid(user);
         return AdminUpdateChecks::NameOfSid(user);
      }

      std::string RandomHex_(size_t bytes)
      {
         std::vector<unsigned char> random(bytes);
         if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(), (ULONG) random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return std::string();
         const char *digits = "0123456789abcdef";
         std::string hex;
         for (unsigned char b : random)
         {
            hex += digits[b >> 4];
            hex += digits[b & 0x0F];
         }
         return hex;
      }

      bool WriteAll_(HANDLE file, const std::string &bytes, bool fromStart)
      {
         if (fromStart)
         {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
               return false;
         }
         size_t offset = 0;
         while (offset < bytes.size())
         {
            DWORD written = 0;
            if (!WriteFile(file, bytes.data() + offset, (DWORD) (bytes.size() - offset), &written, nullptr) || written == 0)
               return false;
            offset += written;
         }
         if (fromStart && !SetEndOfFile(file))
            return false;
         return FlushFileBuffers(file) != FALSE;
      }

      // ------------------------------------------------------------------------- the frame

      void PutU32_(std::string &out, uint32_t value)
      {
         for (int i = 0; i < 4; i++)
            out += (char) ((value >> (8 * i)) & 0xFF);
      }

      void PutU64_(std::string &out, uint64_t value)
      {
         for (int i = 0; i < 8; i++)
            out += (char) ((value >> (8 * i)) & 0xFF);
      }

      void PutString_(std::string &out, const std::string &value)
      {
         PutU32_(out, (uint32_t) value.size());
         out += value;
      }

      class Reader_
      {
      public:
         explicit Reader_(const std::string &data) : data_(data), offset_(0) {}

         bool U8(unsigned char &value)
         {
            if (data_.size() - offset_ < 1)
               return false;
            value = (unsigned char) data_[offset_++];
            return true;
         }

         bool U32(uint32_t &value)
         {
            if (data_.size() - offset_ < 4)
               return false;
            value = 0;
            for (int i = 0; i < 4; i++)
               value |= (uint32_t) (unsigned char) data_[offset_ + i] << (8 * i);
            offset_ += 4;
            return true;
         }

         bool U64(uint64_t &value)
         {
            if (data_.size() - offset_ < 8)
               return false;
            value = 0;
            for (int i = 0; i < 8; i++)
               value |= (uint64_t) (unsigned char) data_[offset_ + i] << (8 * i);
            offset_ += 8;
            return true;
         }

         bool String(std::string &value, size_t most)
         {
            uint32_t length = 0;
            if (!U32(length) || length > most || length > data_.size() - offset_)
               return false;
            value.assign(data_, offset_, length);
            offset_ += length;
            return true;
         }

         bool Raw(char *into, size_t length)
         {
            if (data_.size() - offset_ < length)
               return false;
            memcpy(into, data_.data() + offset_, length);
            offset_ += length;
            return true;
         }

         bool AtEnd() const { return offset_ == data_.size(); }

      private:
         const std::string &data_;
         size_t offset_;
      };

      // ------------------------------------------------------------------------- the stage's files

      // status.txt and stage.log, in the work folder, which the Control Panel's
      // user may read and nobody but administrators may write. status.txt is
      // rewritten whole at every step, key=value per line, state first.
      class StageFiles_
      {
      public:
         StageFiles_() : status_(INVALID_HANDLE_VALUE), log_(INVALID_HANDLE_VALUE) {}
         // Releases what it holds when it goes: a copy would release it twice.
         StageFiles_(const StageFiles_ &) = delete;
         StageFiles_ &operator=(const StageFiles_ &) = delete;
         ~StageFiles_()
         {
            if (status_ != INVALID_HANDLE_VALUE)
               CloseHandle(status_);
            if (log_ != INVALID_HANDLE_VALUE)
               CloseHandle(log_);
         }

         bool Open(const std::wstring &work, const std::vector<std::wstring> &readers, std::wstring &why)
         {
            status_ = AdminUpdateChecks::CreateAdminFile(Join_(work, L"status.txt"), readers, FILE_GENERIC_READ, std::wstring(), why);
            if (status_ == INVALID_HANDLE_VALUE)
               return false;
            log_ = AdminUpdateChecks::CreateAdminFile(Join_(work, L"stage.log"), readers, FILE_GENERIC_READ, std::wstring(), why);
            if (log_ == INVALID_HANDLE_VALUE)
               return false;
            for (const std::wstring &line : early_)
               WriteAll_(log_, Utf8_(line), false);
            early_.clear();
            return true;
         }

         void Log(const std::wstring &line)
         {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t stamp[32];
            swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            const std::wstring text = stamp + OneLine_(line) + L"\r\n";
            if (log_ == INVALID_HANDLE_VALUE)
               early_.push_back(text);
            else
               WriteAll_(log_, Utf8_(text), false);
         }

         void Set(const std::wstring &key, const std::wstring &value)
         {
            values_[key] = OneLine_(value);
         }

         void State(const std::wstring &state)
         {
            Set(L"state", state);
            Set(L"updated", UtcNow_());
            Log(L"State: " + state);
            Flush_();
         }

         void Refused(const std::wstring &check, const std::wstring &reason)
         {
            Set(L"check", check);
            Set(L"reason", reason);
            Log(L"Refused (" + check + L"): " + reason);
            State(L"refused");
         }

      private:
         void Flush_()
         {
            if (status_ == INVALID_HANDLE_VALUE)
               return;
            std::wstring text = L"state=" + values_[L"state"] + L"\r\n";
            for (auto &item : values_)
            {
               if (item.first != L"state")
                  text += item.first + L"=" + item.second + L"\r\n";
            }
            WriteAll_(status_, Utf8_(text), true);
         }

         HANDLE status_;
         HANDLE log_;
         std::vector<std::wstring> early_;
         std::map<std::wstring, std::wstring> values_;
      };

      // One of the stage's copies: its path, and a handle that holds it shared for
      // reading only.
      struct HeldCopy_
      {
         HeldCopy_() : held(INVALID_HANDLE_VALUE), size(0) {}
         ~HeldCopy_() { Release(); }
         // Releases what it holds when it goes: a copy would release it twice.
         HeldCopy_(const HeldCopy_ &) = delete;
         HeldCopy_ &operator=(const HeldCopy_ &) = delete;
         void Release()
         {
            if (held != INVALID_HANDLE_VALUE)
               CloseHandle(held);
            held = INVALID_HANDLE_VALUE;
         }

         std::wstring path;
         HANDLE held;
         unsigned long long size;
      };

      // The source copied into a file made new at destination, then held for
      // reading. The write handle is closed before the hold is taken: a holder
      // with write access would stop the Updater opening it as it must, shared
      // for reading only. Between the two nobody but an administrator could
      // reach the file, which is in the work folder.
      bool CopyAndHold_(const std::wstring &source, const std::wstring &expectedFinal, const std::wstring &destination, unsigned long long cap,
                        HeldCopy_ &copy, std::wstring &why)
      {
         HANDLE file = AdminUpdateChecks::CreateAdminFile(destination, std::vector<std::wstring>(), 0, std::wstring(), why);
         if (file == INVALID_HANDLE_VALUE)
            return false;
         unsigned long long copied = 0;
         const bool ok = AdminUpdateChecks::SafeCopy(source, expectedFinal, cap, file, copied, why);
         CloseHandle(file);
         if (!ok)
            return false;
         copy.path = destination;
         copy.size = copied;
         copy.held = AdminUpdateChecks::HoldForReading(destination, why);
         return copy.held != INVALID_HANDLE_VALUE;
      }

      bool ReadHeld_(HANDLE held, std::string &bytes, size_t cap)
      {
         bytes.clear();
         LARGE_INTEGER zero;
         zero.QuadPart = 0;
         if (!SetFilePointerEx(held, zero, nullptr, FILE_BEGIN))
            return false;
         char buffer[65536];
         for (;;)
         {
            DWORD got = 0;
            if (!ReadFile(held, buffer, sizeof(buffer), &got, nullptr))
               return false;
            if (got == 0)
               return true;
            if (bytes.size() + got > cap)
               return false;
            bytes.append(buffer, got);
         }
      }

      std::vector<unsigned char> FromHex_(const std::string &hex)
      {
         std::vector<unsigned char> out;
         for (size_t i = 0; i + 1 < hex.size(); i += 2)
         {
            auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
            out.push_back((unsigned char) ((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
         }
         return out;
      }

      // Everything the stage checks of one installer, on its held copy: the
      // publisher, the version resource and architecture, the order against
      // this build, the SHA-256 and the Sigstore bundle - the last in the lowered
      // child. expectNewer: the installer must be newer than this build (the
      // update); otherwise it must be this build's own release (the rollback
      // image). check names the check that refused, and why says how.
      struct InstallerFacts_
      {
         InstallerFacts_() : revocationOffline(false) {}

         std::string sha256;
         std::wstring signer;
         std::wstring issuingCa;
         std::wstring architecture;
         std::string identity;
         std::string integratedTime;
         std::wstring crlHost;
         bool revocationOffline;
      };

      bool CheckInstaller_(const HeldCopy_ &installer, const HeldCopy_ &bundle, const std::wstring &version, bool expectNewer,
                           const AuthenticodePublisher::Pin &pin, const SigstoreTrust &trust, const std::wstring &self,
                           StageFiles_ &files, InstallerFacts_ &facts, std::wstring &check, std::wstring &why)
      {
         // (b) Authenticode and the publisher pin, on the held bytes, revocation
         // online and bounded.
         AuthenticodePublisher::Result authenticode;
         if (!AuthenticodePublisher::VerifyHeld(installer.held, installer.path, pin, true, RevocationTimeoutMilliseconds_, authenticode))
         {
            check = L"authenticode";
            why = authenticode.why;
            facts.revocationOffline = authenticode.outcome == AuthenticodePublisher::RevocationOffline ||
                                      authenticode.outcome == AuthenticodePublisher::RevocationTimedOut;
            facts.crlHost = AuthenticodePublisher::HostOf(authenticode.signer.crlUrl);
            files.Log(L"Authenticode: " + std::wstring(AuthenticodePublisher::OutcomeName(authenticode.outcome)) + L" (WinVerifyTrust " +
                      std::to_wstring(authenticode.status) + L"); signer " + authenticode.signer.subjectText + L"; pin: " + authenticode.pin);
            return false;
         }
         facts.signer = authenticode.signer.subjectText;
         facts.issuingCa = authenticode.signer.chainNames.size() > 1 ? authenticode.signer.chainNames[1] : std::wstring();
         std::wstring chain;
         for (const std::wstring &name : authenticode.signer.chainNames)
            chain += (chain.empty() ? L"" : L" <- ") + name;
         std::wstring ekus;
         for (const std::string &eku : authenticode.signer.ekus)
            ekus += (ekus.empty() ? L"" : L", ") + Wide_(eku);
         files.Log(L"Authenticode: Valid, revocation checked online; signer " + facts.signer + L"; chain " + chain + L"; EKUs " + ekus +
                   L"; timestamped; " + std::to_wstring(authenticode.signer.secondarySignatures) + L" secondary signature(s); pin: " + authenticode.pin);

         // (c) The version resource, and the architecture it names.
         String versionWhy;
         if (!UpdateDownloader::InstallerVersionIs(String(installer.path.c_str()), String(version.c_str()), versionWhy))
         {
            check = L"version";
            why = versionWhy.c_str();
            return false;
         }
         std::wstring fixedVersion, description, resourceWhy;
         if (!AdminUpdateChecks::ReadVersionResource(installer.path, fixedVersion, description, resourceWhy))
         {
            check = L"architecture";
            why = L"the installer's " + resourceWhy;
            return false;
         }
         const AdminUpdateChecks::Architecture architecture = AdminUpdateChecks::ArchitectureFromDescription(description);
         facts.architecture = AdminUpdateChecks::ArchitectureName(architecture);
         if (architecture != AdminUpdateChecks::BuildArchitecture())
         {
            check = L"architecture";
            why = L"the installer is for " + facts.architecture + L" (\"" + description + L"\"), and this server is " +
                  AdminUpdateChecks::ArchitectureName(AdminUpdateChecks::BuildArchitecture());
            return false;
         }
         files.Log(L"Version resource: " + fixedVersion + L", \"" + description + L"\": " + facts.architecture);

         // (d) Newer than this build, for the update; this build's own release,
         // for the rollback image.
         const std::wstring running = Wide_(HMAILSERVER_VERSION);
         const int order = AdminUpdateChecks::CompareVersions(version, running);
         if (expectNewer ? order <= 0 : order != 0)
         {
            check = L"version";
            why = expectNewer ? L"hMailServer " + version + L" is not newer than " + running + L", which is installed"
                              : L"the rollback image is hMailServer " + version + L", not " + running + L", which is installed";
            return false;
         }

         // (e) The SHA-256 of the held bytes, which the Sigstore check and the
         // Updater are both held to.
         std::wstring hashWhy;
         facts.sha256 = AdminUpdateChecks::Sha256Of(installer.held, hashWhy);
         if (facts.sha256.size() != 64)
         {
            check = L"sha256";
            why = hashWhy;
            return false;
         }
         files.Log(L"SHA-256: " + Wide_(facts.sha256));

         // (f) Sigstore, in the lowered child: the one parse of bytes the service
         // composed, done without the administrator's rights. Its answer is
         // believed only as exit 0 and one well-formed line, and it cannot stand
         // in for the pin above, which this process checked with Windows' own code.
         std::string bundleBytes;
         if (!ReadHeld_(bundle.held, bundleBytes, (size_t) BundleCap_))
         {
            check = L"sigstore";
            why = L"the Sigstore bundle could not be read";
            return false;
         }
         LoweredChild::Request request;
         request.program = self;
         request.arguments = VerifyBundleVerb_;
         request.input = AdminUpdate::MakeVerifyRequest(String(version.c_str()), FromHex_(facts.sha256), trust,
                                                        AnsiString(bundleBytes.data(), (int) bundleBytes.size()));
         request.timeoutMilliseconds = ChildTimeoutMilliseconds_;
         LoweredChild::Result result;
         if (!LoweredChild::Run(request, result))
         {
            check = L"sigstore";
            why = L"the verifier could not be started with its rights lowered: " + result.error;
            return false;
         }
         // A child that was killed at the timeout, or said more than an answer
         // is, is not believed whatever it had written: its exit code and its
         // output are then the job's and the pipe's, not an answer.
         std::string answerWhy;
         if (result.timedOut || result.outputTruncated ||
             !AdminUpdate::ReadVerifyAnswer(result.exitCode, result.output, facts.identity, facts.integratedTime, answerWhy))
         {
            facts.identity.clear();
            facts.integratedTime.clear();
            check = L"sigstore";
            why = result.timedOut ? L"the Sigstore verifier did not answer within two minutes"
                : result.outputTruncated ? L"the Sigstore verifier's answer was longer than an answer is" : Wide_(answerWhy);
            return false;
         }
         files.Log(L"Sigstore: verified, signer " + Wide_(facts.identity) + L", recorded in the transparency log at " + Wide_(facts.integratedTime));
         return true;
      }

      // The Control Panel that asked: the parent Windows gave this process (the
      // caller of ShellExecuteEx "runas"), named hMailCP.exe at {app}\ControlPanel,
      // started before this process. Its handle, and its user's SID - the reader of
      // the status and outcome files.
      bool CheckParent_(unsigned long expected, const std::wstring &app, HANDLE &parent, std::wstring &readerSid, std::wstring &readerName, std::wstring &why)
      {
         parent = nullptr;
         typedef NTSTATUS(NTAPI * QueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
         HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
         QueryInformationProcess query = ntdll ? (QueryInformationProcess) GetProcAddress(ntdll, "NtQueryInformationProcess") : nullptr;
         if (!query)
         {
            why = L"the process that started this one cannot be asked for";
            return false;
         }

         // PROCESS_BASIC_INFORMATION as the kernel fills it; winternl.h calls the
         // parent's id Reserved3.
         struct BasicInformation_
         {
            NTSTATUS exitStatus;
            PVOID peb;
            ULONG_PTR affinity;
            LONG basePriority;
            ULONG_PTR uniqueProcessId;
            ULONG_PTR inheritedFromUniqueProcessId;
         } information;
         memset(&information, 0, sizeof(information));
         ULONG length = 0;
         if (query(GetCurrentProcess(), ProcessBasicInformation, &information, sizeof(information), &length) < 0)
         {
            why = L"the process that started this one could not be read";
            return false;
         }
         if (information.inheritedFromUniqueProcessId != expected)
         {
            why = L"this was started by process " + std::to_wstring((unsigned long long) information.inheritedFromUniqueProcessId) +
                  L", not by the Control Panel it names (" + std::to_wstring(expected) + L")";
            return false;
         }

         // A standard user's Control Panel, where an administrator's credentials
         // were typed at the prompt, is another user's process. SeDebugPrivilege is
         // on while it and its token are read, and off again however this returns,
         // so that it is not left enabled for the rest of the stage and for what the
         // stage starts.
         SetPrivilege_(SE_DEBUG_NAME, true);
         struct DebugPrivilegeOff_
         {
            ~DebugPrivilegeOff_() { SetPrivilege_(SE_DEBUG_NAME, false); }
         } debugPrivilegeOff;
         parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, expected);
         if (!parent)
         {
            why = L"the Control Panel's process could not be opened (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }

         std::vector<wchar_t> image(32768);
         DWORD imageLength = (DWORD) image.size();
         const std::wstring expectedImage = Join_(Join_(app, L"ControlPanel"), L"hMailCP.exe");
         if (!QueryFullProcessImageNameW(parent, 0, image.data(), &imageLength) ||
             !AdminUpdateChecks::SamePath(LongPath_(std::wstring(image.data(), imageLength)), LongPath_(expectedImage)))
         {
            why = L"the process that started this one is " + std::wstring(image.data(), imageLength) + L", not " + expectedImage;
            CloseHandle(parent);
            parent = nullptr;
            return false;
         }

         FILETIME parentCreated, ownCreated, ignored1, ignored2, ignored3;
         if (!GetProcessTimes(parent, &parentCreated, &ignored1, &ignored2, &ignored3) ||
             !GetProcessTimes(GetCurrentProcess(), &ownCreated, &ignored1, &ignored2, &ignored3) ||
             CompareFileTime(&parentCreated, &ownCreated) > 0)
         {
            why = L"the Control Panel's process started after this one, so it is not the one that asked";
            CloseHandle(parent);
            parent = nullptr;
            return false;
         }

         HANDLE token = nullptr;
         if (OpenProcessToken(parent, TOKEN_QUERY, &token))
         {
            readerName = TokenUserOf_(token, readerSid);
            CloseHandle(token);
         }
         if (readerSid.empty())
         {
            why = L"the Control Panel's user could not be read, so the files that say how the install goes could not be made readable to it";
            CloseHandle(parent);
            parent = nullptr;
            return false;
         }
         return true;
      }

      // The older work folders, but the newest WorkFoldersKept_: removed, links
      // and all, never through a link (the folder is administrators' alone, so
      // only an administrator could have put one there). The handoff's entropy is
      // removed from those kept.
      void SweepWorkFolders_(const std::wstring &adminUpdate, StageFiles_ &files)
      {
         struct Found
         {
            std::wstring path;
            ULONGLONG created;
         };
         std::vector<Found> folders;
         WIN32_FIND_DATAW data;
         HANDLE search = FindFirstFileExW(Join_(adminUpdate, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
         if (search == INVALID_HANDLE_VALUE)
            return;
         do
         {
            const std::wstring name = data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || !AdminUpdateChecks::IsWorkId(name))
               continue;
            Found item;
            item.path = Join_(adminUpdate, name);
            item.created = ((ULONGLONG) data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
            folders.push_back(item);
         } while (FindNextFileW(search, &data));
         FindClose(search);

         std::sort(folders.begin(), folders.end(), [](const Found &a, const Found &b) { return a.created > b.created; });

         std::function<void(const std::wstring &)> remove = [&](const std::wstring &path)
         {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
               return;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            {
               WIN32_FIND_DATAW entry;
               HANDLE inner = FindFirstFileExW(Join_(path, L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
               if (inner != INVALID_HANDLE_VALUE)
               {
                  do
                  {
                     const std::wstring name = entry.cFileName;
                     if (name != L"." && name != L"..")
                        remove(Join_(path, name));
                  } while (FindNextFileW(inner, &entry));
                  FindClose(inner);
               }
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
               RemoveDirectoryW(path.c_str());
            else
               DeleteFileW(path.c_str());
         };

         for (size_t i = 0; i < folders.size(); i++)
         {
            if ((int) i < WorkFoldersKept_)
            {
               DeleteFileW(Join_(folders[i].path, L"handoff-entropy").c_str());
               continue;
            }
            remove(folders[i].path);
            files.Log(L"Removed an older work folder: " + folders[i].path);
         }
      }

      // What --update-* on the command line says, checked against its pattern.
      struct Arguments_
      {
         Arguments_() : controlPanel(0), wait(180) {}

         std::wstring version;
         std::wstring work;
         unsigned long controlPanel;
         unsigned long wait;
      };

      bool ParseArguments_(Arguments_ &arguments)
      {
         int count = 0;
         LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
         if (argv == nullptr)
            return false;
         bool ok = count >= 2 && lstrcmpiW(argv[count - 1], InstallUpdateVerb_) == 0 && (count % 2) == 0;
         bool version = false, work = false, parent = false;
         // argv[0] is the program and argv[count - 1] the verb; the switches are
         // the pairs between, which the even count guarantees.
         for (int i = 1; ok && i < count - 1; i += 2)
         {
            const std::wstring name = argv[i];
            const std::wstring value = argv[i + 1];
            if (name == L"--update-version" && !version)
            {
               ok = AdminUpdateChecks::IsReleaseVersion(value);
               arguments.version = value;
               version = true;
            }
            else if (name == L"--update-work" && !work)
            {
               ok = AdminUpdateChecks::IsWorkId(value);
               arguments.work = value;
               work = true;
            }
            else if (name == L"--control-panel" && !parent)
            {
               ok = AdminUpdateChecks::IsDecimal(value, arguments.controlPanel) && arguments.controlPanel != 0;
               parent = true;
            }
            else if (name == L"--update-wait")
            {
               unsigned long seconds = 0;
               ok = AdminUpdateChecks::IsDecimal(value, seconds);
               arguments.wait = std::min<unsigned long>(3600, std::max<unsigned long>(30, seconds));
            }
            else
               ok = false;
         }
         LocalFree(argv);
         return ok && version && work && parent;
      }

      // Duplicates one of this process's handles into another, with the rights
      // given; the value in that process, or 0.
      unsigned long long DuplicateInto_(HANDLE process, HANDLE handle, DWORD rights)
      {
         HANDLE copy = nullptr;
         if (!DuplicateHandle(GetCurrentProcess(), handle, process, &copy, rights, FALSE, 0))
            return 0;
         return (unsigned long long) (uintptr_t) copy;
      }

      std::wstring Quote_(const std::wstring &text)
      {
         return L"\"" + text + L"\"";
      }
   }

   bool
   AdminUpdate::IsInstallUpdateCommandLine()
   {
      int count = 0;
      LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
      if (argv == nullptr)
         return false;
      const bool is = count >= 2 && lstrcmpiW(argv[count - 1], InstallUpdateVerb_) == 0;
      LocalFree(argv);
      return is;
   }

   bool
   AdminUpdate::IsVerifyBundleCommandLine()
   {
      int count = 0;
      LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &count);
      if (argv == nullptr)
         return false;
      const bool is = count == 2 && lstrcmpiW(argv[1], VerifyBundleVerb_) == 0;
      LocalFree(argv);
      return is;
   }

   String
   AdminUpdate::AdminUpdateFolder()
   {
      // The service's own binary is {app}\Bin\hMailServer.exe.
      const std::wstring bin = DirectoryOf_(ModulePath_());
      const std::wstring app = DirectoryOf_(bin);
      if (app.empty() || _wcsicmp(NameOf_(bin).c_str(), L"Bin") != 0)
         return String();
      return String(Join_(app, L"AdminUpdate").c_str());
   }

   String
   AdminUpdate::LastInstallPath()
   {
      const String folder = AdminUpdateFolder();
      return folder.IsEmpty() ? folder : folder + _T("\\last-install.txt");
   }

   String
   AdminUpdate::InProgressPath()
   {
      const String folder = AdminUpdateFolder();
      return folder.IsEmpty() ? folder : folder + _T("\\in-progress");
   }

   String
   AdminUpdate::ReportedIdPath()
   {
      const String directory = UpdateDownloader::UpdatesDirectory();
      return directory.IsEmpty() ? directory : directory + _T("\\last-admin-install.reported");
   }

   std::string
   AdminUpdate::MakeVerifyRequest(const String &version, const std::vector<unsigned char> &sha256, const SigstoreTrust &trust, const AnsiString &bundle)
   {
      std::string out(RequestMagic_, 4);
      out += (char) RequestFormat_;
      PutString_(out, Utf8_(std::wstring(version.c_str())));
      PutString_(out, std::string(sha256.begin(), sha256.end()));
      PutU32_(out, (uint32_t) trust.ca_pems.size());
      for (const std::string &pem : trust.ca_pems)
         PutString_(out, pem);
      PutString_(out, trust.log_key_pem);
      PutU32_(out, (uint32_t) trust.signers.size());
      for (const SigstoreSigner &signer : trust.signers)
      {
         PutString_(out, std::string(signer.identity_prefix.c_str(), signer.identity_prefix.GetLength()));
         PutString_(out, std::string(signer.issuer.c_str(), signer.issuer.GetLength()));
         PutString_(out, std::string(signer.repository.c_str(), signer.repository.GetLength()));
         PutString_(out, std::string(signer.repository_id.c_str(), signer.repository_id.GetLength()));
         out += (char) (signer.repository_id_optional ? 1 : 0);
         PutU64_(out, (uint64_t) signer.issued_before);
         PutString_(out, std::string(signer.last_version.c_str(), signer.last_version.GetLength()));
         out += (char) (signer.version_in_ref ? 1 : 0);
      }
      PutString_(out, std::string(bundle.c_str(), bundle.GetLength()));
      return out;
   }

   bool
   AdminUpdate::ParseVerifyRequest(const std::string &request, String &version, std::vector<unsigned char> &sha256, SigstoreTrust &trust,
                                   AnsiString &bundle, std::string &why)
   {
      trust = SigstoreTrust();
      if (request.size() > RequestCap_)
      {
         why = "the request is larger than a request may be";
         return false;
      }
      Reader_ reader(request);
      char magic[4];
      unsigned char format = 0;
      if (!reader.Raw(magic, 4) || memcmp(magic, RequestMagic_, 4) != 0 || !reader.U8(format) || format != RequestFormat_)
      {
         why = "the request is not a --verify-bundle request of this version";
         return false;
      }

      std::string text;
      if (!reader.String(text, 64) || !AdminUpdateChecks::IsReleaseVersion(Wide_(text)))
      {
         why = "the request's release is missing or malformed";
         return false;
      }
      version = String(Wide_(text).c_str());

      std::string digest;
      if (!reader.String(digest, 32) || digest.size() != 32)
      {
         why = "the request's digest is not a SHA-256";
         return false;
      }
      sha256.assign(digest.begin(), digest.end());

      uint32_t count = 0;
      if (!reader.U32(count) || count == 0 || count > ListCap_)
      {
         why = "the request's certificate authorities are missing or too many";
         return false;
      }
      for (uint32_t i = 0; i < count; i++)
      {
         std::string pem;
         if (!reader.String(pem, 256 * 1024))
         {
            why = "a certificate authority in the request is malformed";
            return false;
         }
         trust.ca_pems.push_back(pem);
      }
      if (!reader.String(trust.log_key_pem, 64 * 1024))
      {
         why = "the request's log key is malformed";
         return false;
      }
      if (!reader.U32(count) || count == 0 || count > ListCap_)
      {
         why = "the request's signers are missing or too many";
         return false;
      }
      for (uint32_t i = 0; i < count; i++)
      {
         std::string identity, issuer, repository, repositoryId, lastVersion;
         unsigned char optional = 0, inRef = 0;
         uint64_t before = 0;
         if (!reader.String(identity, 4096) || !reader.String(issuer, 4096) || !reader.String(repository, 4096) || !reader.String(repositoryId, 256) ||
             !reader.U8(optional) || optional > 1 || !reader.U64(before) || !reader.String(lastVersion, 64) || !reader.U8(inRef) || inRef > 1)
         {
            why = "a signer in the request is malformed";
            return false;
         }
         SigstoreSigner signer;
         signer.identity_prefix = AnsiString(identity.data(), (int) identity.size());
         signer.issuer = AnsiString(issuer.data(), (int) issuer.size());
         signer.repository = AnsiString(repository.data(), (int) repository.size());
         signer.repository_id = AnsiString(repositoryId.data(), (int) repositoryId.size());
         signer.repository_id_optional = optional == 1;
         signer.issued_before = (__int64) before;
         signer.last_version = AnsiString(lastVersion.data(), (int) lastVersion.size());
         signer.version_in_ref = inRef == 1;
         trust.signers.push_back(signer);
      }

      std::string bundleBytes;
      if (!reader.String(bundleBytes, StringCap_) || bundleBytes.empty())
      {
         why = "the request's bundle is missing or too large";
         return false;
      }
      if (!reader.AtEnd())
      {
         why = "the request has bytes after its end";
         return false;
      }
      bundle = AnsiString(bundleBytes.data(), (int) bundleBytes.size());
      return true;
   }

   bool
   AdminUpdate::ReadVerifyAnswer(unsigned long exitCode, const std::string &output, std::string &identity, std::string &integratedTime, std::string &why)
   {
      identity.clear();
      integratedTime.clear();
      std::string line = output;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
         line.pop_back();

      if (exitCode != 0)
      {
         const std::string said = line.compare(0, 8, "refused ") == 0 ? line.substr(8) : std::string();
         why = said.empty() ? "the Sigstore verifier ended with exit code " + std::to_string(exitCode) + " and no reason"
                            : "the Sigstore bundle did not verify: " + said.substr(0, 1000);
         return false;
      }
      if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos || line.compare(0, 9, "verified ") != 0)
      {
         why = "the Sigstore verifier's answer is not one \"verified\" line";
         return false;
      }
      const std::string rest = line.substr(9);
      const size_t space = rest.find(' ');
      if (space == std::string::npos || space == 0)
      {
         why = "the Sigstore verifier's answer names no signer";
         return false;
      }
      identity = rest.substr(0, space);
      integratedTime = rest.substr(space + 1);
      if (integratedTime.empty() || integratedTime.size() > 32 || identity.size() > 2048 ||
          identity.find_first_of(" \t\"<>") != std::string::npos || integratedTime.find_first_not_of("0123456789-T:Z") != std::string::npos)
      {
         identity.clear();
         integratedTime.clear();
         why = "the Sigstore verifier's answer is malformed";
         return false;
      }
      return true;
   }

   int
   AdminUpdate::RunVerifyBundleChild()
   {
      // Before anything that could: no line of the logger or of ErrorManager
      // from this process, and DLLs from Bin and System32 alone.
      Logger::ForbidWritesForThisProcess();
      SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

      HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
      HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
      auto answer = [&](const std::string &line, int code) -> int
      {
         std::string text = line;
         for (char &c : text)
         {
            if (c == '\r' || c == '\n')
               c = ' ';
         }
         text += "\n";
         DWORD written = 0;
         if (output != nullptr && output != INVALID_HANDLE_VALUE)
            WriteFile(output, text.data(), (DWORD) text.size(), &written, nullptr);
         return code;
      };

      std::string request;
      char buffer[65536];
      for (;;)
      {
         DWORD got = 0;
         if (input == nullptr || input == INVALID_HANDLE_VALUE || !ReadFile(input, buffer, sizeof(buffer), &got, nullptr) || got == 0)
            break;
         if (request.size() + got > RequestCap_)
            return answer("refused the request is larger than a request may be", 2);
         request.append(buffer, got);
      }

      String version;
      std::vector<unsigned char> sha256;
      SigstoreTrust trust;
      AnsiString bundle;
      std::string why;
      if (!ParseVerifyRequest(request, version, sha256, trust, bundle, why))
         return answer("refused malformed request: " + why, 2);

      SigstoreVerdict verdict;
      if (!SigstoreVerifier::VerifyDigest(sha256, bundle, trust, verdict, version))
         return answer("refused " + Utf8_(std::wstring(verdict.error.c_str())), 1);

      return answer("verified " + std::string(verdict.identity.c_str()) + " " + std::string(Utf8_(std::wstring(UpdateChecker::FormatUnixTime(verdict.integrated_time).c_str()))), 0);
   }

   int
   AdminUpdate::RunInstallUpdateCommand()
   {
      // Before anything that could: nothing written by the logger or
      // ErrorManager from this process, and DLLs from Bin (checked below) and
      // System32 alone - never the current directory or PATH.
      Logger::ForbidWritesForThisProcess();
      SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

      StageFiles_ files;
      files.Log(L"hMailServer.exe /InstallUpdate, hMailServer " + Wide_(HMAILSERVER_VERSION) + L", process " + std::to_wstring(GetCurrentProcessId()));

      // 1. The command line: known switches, each to its pattern. No path.
      Arguments_ arguments;
      if (!ParseArguments_(arguments))
         return StageArguments;

      // 2. An administrator's elevated token.
      if (!IsElevatedAdministrator_())
         return StageNotElevated;

      // 3. This is the service's own program, in Bin: {app} is the folder above.
      const std::wstring self = ModulePath_();
      const std::wstring registered = ServiceBinary_();
      if (self.empty() || registered.empty() || !AdminUpdateChecks::SamePath(LongPath_(self), LongPath_(registered)))
         return StageArguments;
      const std::wstring bin = DirectoryOf_(LongPath_(self));
      const std::wstring app = DirectoryOf_(bin);
      if (app.empty() || _wcsicmp(NameOf_(bin).c_str(), L"Bin") != 0)
         return StageArguments;

      // 4. hMailServer.INI, read from the file alone as the service reads the
      // trust (docs/UpdateFeed.md): the data folder, and the five trust
      // settings. Nothing from it is used before the permission rule below has
      // passed it.
      const std::wstring ini = Join_(bin, L"hMailServer.ini");
      auto readIni = [&](const wchar_t *section, const wchar_t *key) -> std::wstring
      {
         std::vector<wchar_t> value(4096);
         const DWORD length = GetPrivateProfileStringW(section, key, L"", value.data(), (DWORD) value.size(), ini.c_str());
         std::wstring text(value.data(), length);
         while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
         while (!text.empty() && (text.front() == L' ' || text.front() == L'\t'))
            text.erase(text.begin());
         return text;
      };
      SigstoreTrustSettings trustSettings;
      trustSettings.roots_file = readIni(L"Settings", L"UpdateTrustRootsFile").c_str();
      trustSettings.log_key_file = readIni(L"Settings", L"UpdateLogPublicKeyFile").c_str();
      trustSettings.identity = readIni(L"Settings", L"UpdateSigningIdentity").c_str();
      trustSettings.issuer = readIni(L"Settings", L"UpdateSigningIssuer").c_str();
      trustSettings.repository = readIni(L"Settings", L"UpdateSourceRepository").c_str();
      std::vector<std::wstring> trustFiles;
      if (!trustSettings.roots_file.IsEmpty())
         trustFiles.push_back(std::wstring(trustSettings.roots_file.c_str()));
      if (!trustSettings.log_key_file.IsEmpty())
         trustFiles.push_back(std::wstring(trustSettings.log_key_file.c_str()));

      // 5. The permission rule, again: the Control Panel ran it before it asked
      // for elevation, and this binary could only be run elevated if it held
      // then; it is run here too, because it is what makes everything below
      // trustworthy, and a file's permissions can change in between.
      AdminUpdateChecks::Finding finding;
      if (!AdminUpdateChecks::CheckInstallation(app, trustFiles, finding))
      {
         for (const std::wstring &file : trustFiles)
         {
            if (!finding.object.empty() && AdminUpdateChecks::SamePath(finding.object, file))
               return StageTrustFile;
         }
         return StagePermissions;
      }

      // 6. One administrator's install at a time: a file of the administrators'
      // own folder, held exclusively for as long as the stage and then the
      // Updater live. Only an administrator can make or hold it. The folder's
      // permissions - not what is in it - are readable by anyone, so that the
      // Control Panel of the next install, unelevated, can hold them to the rule
      // before it asks Windows for the elevation.
      const std::wstring adminUpdate = Join_(app, L"AdminUpdate");
      HANDLE adminUpdateHeld = INVALID_HANDLE_VALUE;
      std::wstring why;
      if (!AdminUpdateChecks::MakeOrOpenAdminFolder(adminUpdate, false, std::wstring(), adminUpdateHeld, why, std::vector<std::wstring>(), true))
         return StagePermissions;
      SECURITY_ATTRIBUTES inheritable;
      memset(&inheritable, 0, sizeof(inheritable));
      inheritable.nLength = sizeof(inheritable);
      inheritable.bInheritHandle = TRUE;
      HANDLE busy = CreateFileW(Join_(adminUpdate, L"busy").c_str(), GENERIC_READ | GENERIC_WRITE, 0, &inheritable, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (busy == INVALID_HANDLE_VALUE)
      {
         CloseHandle(adminUpdateHeld);
         return GetLastError() == ERROR_SHARING_VIOLATION ? StageBusy : StageFailed;
      }

      // 7. The Control Panel that asked.
      HANDLE parent = nullptr;
      std::wstring readerSid, readerName;
      if (!CheckParent_(arguments.controlPanel, app, parent, readerSid, readerName, why))
      {
         CloseHandle(busy);
         CloseHandle(adminUpdateHeld);
         return StageParent;
      }
      std::vector<std::wstring> readers;
      readers.push_back(readerSid);

      // 8. The work folder, new, and the files the Control Panel reads.
      SweepWorkFolders_(adminUpdate, files);
      const std::wstring work = Join_(adminUpdate, arguments.work);
      HANDLE workHeld = INVALID_HANDLE_VALUE;
      if (!AdminUpdateChecks::MakeOrOpenAdminFolder(work, true, std::wstring(), workHeld, why))
      {
         CloseHandle(parent);
         CloseHandle(busy);
         CloseHandle(adminUpdateHeld);
         return GetFileAttributesW(work.c_str()) != INVALID_FILE_ATTRIBUTES ? StageNameTaken : StageFailed;
      }
      if (!files.Open(work, readers, why))
      {
         CloseHandle(workHeld);
         CloseHandle(parent);
         CloseHandle(busy);
         CloseHandle(adminUpdateHeld);
         return StageFailed;
      }

      std::wstring ownSid;
      std::wstring ownName;
      {
         HANDLE token = nullptr;
         if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
         {
            ownName = TokenUserOf_(token, ownSid);
            CloseHandle(token);
         }
      }
      files.Set(L"elevated-by", ownName);
      files.Set(L"control-panel-user", readerName);
      files.Set(L"version", arguments.version);
      files.Set(L"from", Wide_(HMAILSERVER_VERSION));
      files.Log(L"Installation " + app + L"; asked by the Control Panel (process " + std::to_wstring(arguments.controlPanel) + L", " + readerName +
                L"); elevated as " + ownName + L"; hMailServer " + arguments.version + L" to replace " + Wide_(HMAILSERVER_VERSION));
      files.State(L"checking");

      // Whatever ends the stage from here: its holds let go, and the files say
      // how it ended.
      struct Holds_
      {
         Holds_(HANDLE first, HANDLE second, HANDLE third, HANDLE fourth) : handles{ first, second, third, fourth } {}
         // Releases what it holds when it goes: a copy would release it twice.
         Holds_(const Holds_ &) = delete;
         Holds_ &operator=(const Holds_ &) = delete;

         HANDLE handles[4];
         ~Holds_()
         {
            for (HANDLE handle : handles)
            {
               if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
                  CloseHandle(handle);
            }
         }
      } holds(workHeld, parent, busy, adminUpdateHeld);

      // 9. The data folder and the trust, from the INI that passed the rule.
      std::wstring programFolder = readIni(L"Directories", L"ProgramFolder");
      std::wstring dataFolder = readIni(L"Directories", L"DataFolder");
      if (!dataFolder.empty() && !(dataFolder.size() > 1 && (dataFolder[1] == L':' || (dataFolder[0] == L'\\' && dataFolder[1] == L'\\'))))
         dataFolder = Join_(programFolder.empty() ? app : programFolder, dataFolder);
      while (dataFolder.size() > 3 && dataFolder.back() == L'\\')
         dataFolder.pop_back();
      if (dataFolder.empty())
      {
         files.Refused(L"configuration", ini + L" names no [Directories] DataFolder, so there is no download to install");
         return StageFailed;
      }
      dataFolder = LongPath_(FullPath_(dataFolder));
      files.Log(L"Data folder: " + dataFolder);

      SigstoreTrust trust;
      String trustError;
      if (!SigstoreVerifier::ConfiguredTrustFrom(trustSettings, trust, trustError))
      {
         files.Refused(L"trust", std::wstring(trustError.c_str()));
         return StageRefused;
      }

      // The pin: this project's publisher, or - where the INI says a site signs
      // its own builds - the signer of this very hMailServer.exe, which only an
      // administrator could have put here.
      AuthenticodePublisher::Pin pin = AuthenticodePublisher::ProjectPin();
      if (!SigstoreVerifier::IsProjectIdentity(trustSettings.identity))
      {
         AuthenticodePublisher::Signer installed;
         std::wstring signerWhy;
         if (!AuthenticodePublisher::SignerOf(self, installed, signerWhy))
         {
            files.Refused(L"site-signer", L"UpdateSigningIdentity names a signer of this site's own, and the installed hMailServer.exe is not "
                                          L"Authenticode-signed (" + signerWhy + L"), so there is no publisher to hold the installer to");
            return StageRefused;
         }
         pin.sameAsInstalled = true;
         pin.installedSubject = installed.subject;
         pin.installedSubjectText = installed.subjectText;
         pin.installedRootSha256 = installed.chainSha256.empty() ? std::string() : installed.chainSha256.back();
      }
      files.Set(L"pin", pin.sameAsInstalled ? L"installed-signer " + pin.installedSubjectText : std::wstring(L"project"));

      // 10. Room for the copies and the installer's own unpacking.
      ULARGE_INTEGER freeBytes;
      if (!GetDiskFreeSpaceExW(app.c_str(), &freeBytes, nullptr, nullptr) || freeBytes.QuadPart < FreeSpaceNeeded_)
      {
         files.Refused(L"disk", L"there is less than 1 GB free on the drive of " + app);
         return StageDiskSpace;
      }

      // 11. The installer: copied out of the service's folder, held, checked.
      const std::wstring installerName = std::wstring(UpdateChecker::PlatformInstallerName(String(arguments.version.c_str())).c_str());
      const std::wstring updates = Join_(dataFolder, L"Updates");
      files.Set(L"installer-source", Join_(updates, installerName));
      HeldCopy_ installer, bundle;
      if (!CopyAndHold_(Join_(updates, installerName), Join_(updates, installerName), Join_(work, installerName), InstallerCap_, installer, why))
      {
         files.Refused(L"copy", why);
         return StageRefused;
      }
      if (!CopyAndHold_(Join_(updates, installerName + L".cosign.bundle"), Join_(updates, installerName + L".cosign.bundle"),
                        Join_(work, installerName + L".cosign.bundle"), BundleCap_, bundle, why))
      {
         files.Refused(L"copy", why);
         return StageRefused;
      }
      files.Log(L"Copied " + installerName + L" (" + std::to_wstring(installer.size) + L" bytes) and its bundle from " + updates + L" into " + work);

      InstallerFacts_ facts;
      std::wstring check;
      if (!CheckInstaller_(installer, bundle, arguments.version, true, pin, trust, self, files, facts, check, why))
      {
         if (facts.revocationOffline)
         {
            files.Set(L"revocation", L"offline");
            files.Set(L"crl-host", facts.crlHost);
         }
         files.Set(L"kept", work);
         files.Refused(check, why);
         return StageRefused;
      }
      files.Set(L"signer", facts.signer);
      files.Set(L"issuing-ca", facts.issuingCa);
      files.Set(L"architecture", facts.architecture);
      files.Set(L"sha256", Wide_(facts.sha256));
      files.Set(L"sigstore-identity", Wide_(facts.identity));
      files.Set(L"log-time", Wide_(facts.integratedTime));

      // 12. The two events the Control Panel sets - ready, when the service has
      // taken its backup and its rollback image, and cancel - made without names,
      // so that nobody could have made them first, and handed to the Control
      // Panel's process with the right to set them and nothing else.
      HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      HANDLE cancel = CreateEventW(&inheritable, TRUE, FALSE, nullptr);
      if (!ready || !cancel)
      {
         files.Refused(L"events", L"the events the Control Panel sets could not be made");
         return StageFailed;
      }
      struct Events_
      {
         Events_(HANDLE first, HANDLE second) : a(first), b(second) {}
         // Releases what it holds when it goes: a copy would release it twice.
         Events_(const Events_ &) = delete;
         Events_ &operator=(const Events_ &) = delete;

         HANDLE a, b;
         ~Events_()
         {
            CloseHandle(a);
            CloseHandle(b);
         }
      } events(ready, cancel);
      const unsigned long long readyInPanel = DuplicateInto_(parent, ready, EVENT_MODIFY_STATE | SYNCHRONIZE);
      const unsigned long long cancelInPanel = DuplicateInto_(parent, cancel, EVENT_MODIFY_STATE | SYNCHRONIZE);
      if (readyInPanel == 0 || cancelInPanel == 0)
      {
         files.Refused(L"events", L"the events could not be handed to the Control Panel (Windows error " + std::to_wstring(GetLastError()) + L")");
         return StageFailed;
      }
      files.Set(L"ready-event", std::to_wstring(readyInPanel));
      files.Set(L"cancel-event", std::to_wstring(cancelInPanel));
      files.Log(L"Installer verified: signed by " + facts.signer + L" (" + facts.issuingCa + L"), Sigstore " + Wide_(facts.identity) + L" at " +
                Wide_(facts.integratedTime) + L", version " + arguments.version + L", " + facts.architecture);
      files.State(L"installer-verified");

      // 13. The service prepares - backup, rollback image - at the Control Panel's
      // request, over COM; this waits, for at most an hour and a half, and stops
      // when the Control Panel cancels or ends.
      {
         HANDLE waits[3] = { ready, cancel, parent };
         const DWORD woke = WaitForMultipleObjects(3, waits, FALSE, PreparationWaitMilliseconds_);
         if (woke != WAIT_OBJECT_0)
         {
            files.Set(L"reason", woke == WAIT_OBJECT_0 + 1 ? L"the Control Panel cancelled" : woke == WAIT_OBJECT_0 + 2 ? L"the Control Panel ended" : L"the service was not ready within 90 minutes");
            files.State(L"cancelled");
            return StageCancelled;
         }
      }
      files.Log(L"The Control Panel says the service is ready.");

      // 14. The rollback image the service fetched, checked as the installer was,
      // against this build's own release. None is not fatal, as in the
      // unattended apply.
      const std::wstring running = Wide_(HMAILSERVER_VERSION);
      HeldCopy_ rollback, rollbackBundle;
      InstallerFacts_ rollbackFacts;
      bool rollbackReady = false;
      {
         const std::wstring rollbackName = std::wstring(UpdateChecker::PlatformInstallerName(String(running.c_str())).c_str());
         const std::wstring rollbackSource = Join_(Join_(updates, L"rollback"), rollbackName);
         const std::wstring rollbackWork = Join_(work, L"rollback");
         HANDLE rollbackHeld = INVALID_HANDLE_VALUE;
         std::wstring rollbackWhy;
         auto prepare = [&]() -> bool
         {
            if (GetFileAttributesW(rollbackSource.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
               rollbackWhy = L"the service has no rollback image for hMailServer " + running;
               return false;
            }
            if (!AdminUpdateChecks::MakeOrOpenAdminFolder(rollbackWork, true, std::wstring(), rollbackHeld, rollbackWhy))
               return false;
            if (!CopyAndHold_(rollbackSource, rollbackSource, Join_(rollbackWork, rollbackName), InstallerCap_, rollback, rollbackWhy) ||
                !CopyAndHold_(rollbackSource + L".cosign.bundle", rollbackSource + L".cosign.bundle", Join_(rollbackWork, rollbackName + L".cosign.bundle"),
                              BundleCap_, rollbackBundle, rollbackWhy))
               return false;
            std::wstring rollbackCheck;
            if (!CheckInstaller_(rollback, rollbackBundle, running, false, pin, trust, self, files, rollbackFacts, rollbackCheck, rollbackWhy))
            {
               rollbackWhy = rollbackCheck + L": " + rollbackWhy;
               return false;
            }
            return true;
         };
         rollbackReady = prepare();
         if (rollbackHeld != INVALID_HANDLE_VALUE)
            CloseHandle(rollbackHeld);

         if (rollbackReady)
         {
            files.Set(L"rollback", L"verified");
            files.Set(L"rollback-version", running);
            files.State(L"rollback-verified");
         }
         else
         {
            rollback.Release();
            files.Set(L"rollback", L"none");
            files.Set(L"rollback-reason", rollbackWhy);
            files.State(L"rollback-none");
         }
      }

      // 15. The Updater: Bin's own, copied beside the installer and held to
      // Bin's SHA-256, since the installer is about to replace Bin.
      HeldCopy_ helper;
      {
         const std::wstring source = Join_(bin, L"hMailServer.Updater.exe");
         if (!CopyAndHold_(source, source, Join_(work, L"hMailServer.Updater.exe"), HelperCap_, helper, why))
         {
            files.Refused(L"helper", why);
            return StageFailed;
         }
         std::wstring hashWhy;
         HANDLE original = AdminUpdateChecks::HoldForReading(source, hashWhy);
         const std::string originalHash = original != INVALID_HANDLE_VALUE ? AdminUpdateChecks::Sha256Of(original, hashWhy) : std::string();
         if (original != INVALID_HANDLE_VALUE)
            CloseHandle(original);
         const std::string copyHash = AdminUpdateChecks::Sha256Of(helper.held, hashWhy);
         if (originalHash.empty() || originalHash != copyHash)
         {
            files.Refused(L"helper", L"the copy of hMailServer.Updater.exe is not Bin's byte for byte");
            return StageFailed;
         }
      }

      // 16. The handoff's secret half, which the Control Panel's sign-in handoff is
      // bound to (Services/AfterUpdateHandoff.cs): readable, and removable, by the
      // Control Panel's user alone, in a folder only administrators can write.
      {
         const std::string entropy = RandomHex_(32);
         HANDLE file = AdminUpdateChecks::CreateAdminFile(Join_(work, L"handoff-entropy"), readers, FILE_GENERIC_READ | DELETE, std::wstring(), why);
         if (file == INVALID_HANDLE_VALUE || entropy.size() != 64 || !WriteAll_(file, entropy, true))
            files.Log(L"The sign-in handoff's entropy could not be written (" + why + L"); the Control Panel will ask for the password again.");
         if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
      }

      // 17. The hand-over: the Updater, elevated as this process is, from the work
      // folder, given the Control Panel's process and the cancel event to wait on
      // and the busy file to hold - those three handles and no other.
      std::wstring helperPath;
      if (!AdminUpdateChecks::FinalPathOf(helper.held, helperPath))
      {
         files.Refused(L"helper", L"Windows could not say where the Updater's copy is");
         return StageFailed;
      }
      HANDLE parentForHelper = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &parentForHelper, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
      {
         files.Refused(L"helper", L"the Control Panel's process could not be handed to the Updater");
         return StageFailed;
      }

      const std::wstring tokenFile = Join_(updates, L"apply-token");
      std::wstring command = Quote_(helperPath);
      command += L" --mode administrator";
      command += L" --work " + Quote_(work);
      command += L" --app " + Quote_(app);
      command += std::wstring(L" --service ") + ServiceName_;
      command += L" --installer " + Quote_(installer.path);
      command += L" --installer-sha256 " + Wide_(facts.sha256);
      command += L" --version " + arguments.version;
      command += L" --from " + running;
      if (rollbackReady)
      {
         command += L" --rollback " + Quote_(rollback.path);
         command += L" --rollback-sha256 " + Wide_(rollbackFacts.sha256);
         command += L" --rollback-version " + running;
      }
      command += L" --wait-process " + std::to_wstring((unsigned long long) (uintptr_t) parentForHelper);
      command += L" --cancel-event " + std::to_wstring((unsigned long long) (uintptr_t) cancel);
      command += L" --busy " + std::to_wstring((unsigned long long) (uintptr_t) busy);
      command += L" --token-file " + Quote_(tokenFile);
      command += L" --outcome " + Quote_(Join_(work, L"outcome.txt"));
      command += L" --admin-outcome " + Quote_(Join_(adminUpdate, L"last-install.txt"));
      command += L" --in-progress " + Quote_(Join_(adminUpdate, L"in-progress"));
      command += L" --log " + Quote_(Join_(work, L"apply.log"));
      command += L" --wait " + std::to_wstring(arguments.wait);
      command += L" --relaunch-id " + arguments.work;
      command += L" --reader " + readerSid;

      SIZE_T attributeSize = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
      std::vector<unsigned char> attributeBuffer(attributeSize ? attributeSize : 1);
      LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) attributeBuffer.data();
      HANDLE inherited[3] = { parentForHelper, cancel, busy };
      bool listed = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
                    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
      if (!listed)
      {
         CloseHandle(parentForHelper);
         files.Refused(L"helper", L"the list of handles the Updater inherits could not be made");
         return StageFailed;
      }

      STARTUPINFOEXW startup;
      memset(&startup, 0, sizeof(startup));
      startup.StartupInfo.cb = sizeof(startup);
      startup.lpAttributeList = attributes;
      PROCESS_INFORMATION process;
      memset(&process, 0, sizeof(process));
      std::vector<wchar_t> line(command.begin(), command.end());
      line.push_back(L'\0');
      DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT | CREATE_BREAKAWAY_FROM_JOB;
      BOOL started = CreateProcessW(helperPath.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, work.c_str(), &startup.StartupInfo, &process);
      if (!started && GetLastError() == ERROR_ACCESS_DENIED)
      {
         flags &= ~CREATE_BREAKAWAY_FROM_JOB;
         started = CreateProcessW(helperPath.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, work.c_str(), &startup.StartupInfo, &process);
      }
      const DWORD startError = GetLastError();
      DeleteProcThreadAttributeList(attributes);
      CloseHandle(parentForHelper);
      if (!started)
      {
         files.Refused(L"helper", L"the Updater could not be started (Windows error " + std::to_wstring(startError) + L")");
         return StageFailed;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);

      files.Set(L"updater-pid", std::to_wstring(process.dwProcessId));
      if (Logger::SuppressedWrites() > 0)
         files.Log(L"Log lines and errors kept from the service's folders: " + std::to_wstring(Logger::SuppressedWrites()));
      files.Log(L"Handed over to the Updater, process " + std::to_wstring(process.dwProcessId) + L": " + command);
      files.State(L"handed-over");
      return StageHandedOver;
   }

   void
   AdminUpdateStageTester::Test()
   {
      auto expect = [](bool condition, const std::string &what)
      {
         if (!condition)
            throw std::logic_error("AdminUpdateStageTester: " + what);
      };

      // The request, made and read back.
      SigstoreTrust trust = SigstoreVerifier::DefaultTrust();
      std::vector<unsigned char> digest(32, 0xAB);
      const AnsiString bundleText = "{\"mediaType\":\"x\"}";
      const std::string request = AdminUpdate::MakeVerifyRequest(_T("6.3.7"), digest, trust, bundleText);
      {
         String version;
         std::vector<unsigned char> readDigest;
         SigstoreTrust readTrust;
         AnsiString readBundle;
         std::string why;
         expect(AdminUpdate::ParseVerifyRequest(request, version, readDigest, readTrust, readBundle, why), "a request did not read back: " + why);
         expect(version == _T("6.3.7") && readDigest == digest && readBundle == bundleText && readTrust.ca_pems == trust.ca_pems &&
                readTrust.log_key_pem == trust.log_key_pem && readTrust.signers.size() == trust.signers.size(), "a request read back differently");
         for (size_t i = 0; i < trust.signers.size(); i++)
         {
            const SigstoreSigner &a = trust.signers[i];
            const SigstoreSigner &b = readTrust.signers[i];
            expect(a.identity_prefix == b.identity_prefix && a.issuer == b.issuer && a.repository == b.repository && a.repository_id == b.repository_id &&
                   a.repository_id_optional == b.repository_id_optional && a.issued_before == b.issued_before && a.last_version == b.last_version &&
                   a.version_in_ref == b.version_in_ref, "a signer read back differently");
         }
      }

      // Every malformed form of it refused.
      {
         String version;
         std::vector<unsigned char> readDigest;
         SigstoreTrust readTrust;
         AnsiString readBundle;
         std::string why;
         expect(!AdminUpdate::ParseVerifyRequest(request.substr(0, request.size() - 1), version, readDigest, readTrust, readBundle, why), "a request cut short was read");
         expect(!AdminUpdate::ParseVerifyRequest(request + "x", version, readDigest, readTrust, readBundle, why), "a request with a byte after its end was read");
         std::string otherMagic = request;
         otherMagic[0] = 'X';
         expect(!AdminUpdate::ParseVerifyRequest(otherMagic, version, readDigest, readTrust, readBundle, why), "a request of another magic was read");
         std::vector<unsigned char> shortDigest(31, 0xAB);
         expect(!AdminUpdate::ParseVerifyRequest(AdminUpdate::MakeVerifyRequest(_T("6.3.7"), shortDigest, trust, bundleText), version, readDigest, readTrust, readBundle, why),
                "a request with a 31-byte digest was read");
         expect(!AdminUpdate::ParseVerifyRequest(AdminUpdate::MakeVerifyRequest(_T("6.3.7 & calc"), digest, trust, bundleText), version, readDigest, readTrust, readBundle, why),
                "a request with a malformed release was read");
         SigstoreTrust many = trust;
         while (many.signers.size() <= 16)
            many.signers.push_back(trust.signers[0]);
         expect(!AdminUpdate::ParseVerifyRequest(AdminUpdate::MakeVerifyRequest(_T("6.3.7"), digest, many, bundleText), version, readDigest, readTrust, readBundle, why),
                "a request with seventeen signers was read");
         // A length field past the frame: the version's length set to four billion.
         std::string huge = request;
         huge[5] = huge[6] = huge[7] = huge[8] = (char) 0xFF;
         expect(!AdminUpdate::ParseVerifyRequest(huge, version, readDigest, readTrust, readBundle, why), "a field longer than its frame was read");
      }

      // The answer: exit 0 and one well-formed line, and nothing else.
      {
         std::string identity, time, why;
         expect(AdminUpdate::ReadVerifyAnswer(0, "verified https://gitlab.com/x//.gitlab-ci.yml@refs/tags/v6.3.7 2026-09-29T09:41:48Z\n", identity, time, why) &&
                identity == "https://gitlab.com/x//.gitlab-ci.yml@refs/tags/v6.3.7" && time == "2026-09-29T09:41:48Z", "a verified answer was not read");
         expect(!AdminUpdate::ReadVerifyAnswer(1, "verified a 2026-09-29T09:41:48Z\n", identity, time, why), "a verified line with exit 1 was believed");
         expect(!AdminUpdate::ReadVerifyAnswer(0, "refused no\n", identity, time, why), "a refused line was believed");
         expect(!AdminUpdate::ReadVerifyAnswer(0, "verified a 2026-09-29T09:41:48Z\nverified b 1\n", identity, time, why), "two lines were believed");
         expect(!AdminUpdate::ReadVerifyAnswer(0, "", identity, time, why), "no answer was believed");
         expect(!AdminUpdate::ReadVerifyAnswer(0, "verified  2026\n", identity, time, why), "an answer naming no signer was believed");
         expect(!AdminUpdate::ReadVerifyAnswer(0, "verified a\"b 2026\n", identity, time, why), "an answer with a quote in the signer was believed");
      }

      // The Updater's outcome file, as the service reads it at its next start.
      {
         UpdateInstaller::AdministratorOutcome outcome;
         expect(UpdateInstaller::ParseAdministratorOutcome("ok 6.3.7 the installer succeeded and the service is running\r\nid=0123456789abcdef0123456789abcdef\r\nby=HOST\\admin\r\nfrom=6.3.6\r\ninstall-log=C:\\x.log\r\n", outcome) &&
                outcome.status == _T("ok") && outcome.version == _T("6.3.7") && outcome.detail == _T("the installer succeeded and the service is running") &&
                outcome.id == _T("0123456789abcdef0123456789abcdef") && outcome.by == _T("HOST\\admin") && outcome.from == _T("6.3.6"),
                "an outcome was not read");
         expect(UpdateInstaller::ParseAdministratorOutcome("rolled-back 6.3.7 x\nid=0123456789abcdef0123456789abcdef\n", outcome) && outcome.status == _T("rolled-back"),
                "a rolled-back outcome with LF line ends was not read");
         // An install whose database step changed the database and that did not
         // complete, which the Updater does not roll back (exit code 11).
         expect(UpdateInstaller::ParseAdministratorOutcome("upgraded-incomplete 6.3.7 the installer exited with 11: a step did not complete\nid=0123456789abcdef0123456789abcdef\n", outcome) &&
                outcome.status == _T("upgraded-incomplete") && outcome.detail == _T("the installer exited with 11: a step did not complete"),
                "an upgraded-incomplete outcome was not read");
         expect(!UpdateInstaller::ParseAdministratorOutcome("installed 6.3.7 x\nid=0123456789abcdef0123456789abcdef\n", outcome), "an unknown status was read");
         expect(!UpdateInstaller::ParseAdministratorOutcome("ok 6.3.7 x\n", outcome), "an outcome with no id was read");
         expect(!UpdateInstaller::ParseAdministratorOutcome("ok 6.3.7 x\nid=0123456789ABCDEF0123456789abcdef\n", outcome), "an outcome with an id in upper case was read");
         expect(!UpdateInstaller::ParseAdministratorOutcome("ok\nid=0123456789abcdef0123456789abcdef\n", outcome), "an outcome with no version was read");
         expect(!UpdateInstaller::ParseAdministratorOutcome("", outcome), "an empty outcome was read");
      }

      // The real 6.3.5 bundle: VerifyDigest and VerifyFile agree, and a digest one
      // bit off is refused.
      const std::wstring fixtures = AdminUpdateTester::FixturesFolder();
      const std::wstring installerPath = Join_(fixtures, L"hMailServer-6.3.5-x64.exe");
      const std::wstring bundlePath = installerPath + L".cosign.bundle";
      for (const std::wstring &fixture : { installerPath, bundlePath })
      {
         const std::wstring problem = AdminUpdateTester::FixtureProblem(fixture);
         expect(problem.empty(), Utf8_(problem));
      }
      AnsiString realBundle;
      expect(SigstoreVerifier::ReadBundle(String(bundlePath.c_str()), realBundle), "the 6.3.5 bundle could not be read");
      std::vector<unsigned char> realDigest;
      String hashError;
      expect(SigstoreVerifier::Sha256File(String(installerPath.c_str()), realDigest, hashError), "the 6.3.5 installer could not be hashed");
      {
         SigstoreVerdict byFile, byDigest;
         const bool file = SigstoreVerifier::VerifyFile(String(installerPath.c_str()), realBundle, trust, byFile, _T("6.3.5"));
         const bool direct = SigstoreVerifier::VerifyDigest(realDigest, realBundle, trust, byDigest, _T("6.3.5"));
         expect(file && direct, "the real 6.3.5 bundle did not verify: " + Utf8_(std::wstring(byDigest.error.c_str())));
         expect(byFile.identity == byDigest.identity && byFile.integrated_time == byDigest.integrated_time, "VerifyFile and VerifyDigest disagree");

         std::vector<unsigned char> flipped = realDigest;
         flipped[17] ^= 0x01;
         SigstoreVerdict refused;
         expect(!SigstoreVerifier::VerifyDigest(flipped, realBundle, trust, refused, _T("6.3.5")), "a digest one bit off verified");
         expect(!SigstoreVerifier::VerifyDigest(realDigest, realBundle, trust, refused, _T("6.3.6")), "6.3.5's bundle verified as 6.3.6");
         expect(!SigstoreVerifier::VerifyDigest(realDigest, realBundle, trust, refused, String()), "a digest verified for no release");
      }

      // hMailServer.exe --verify-bundle itself, lowered, as the stage runs it.
      {
         LoweredChild::Request verify;
         verify.program = ModulePath_();
         verify.arguments = VerifyBundleVerb_;
         verify.input = AdminUpdate::MakeVerifyRequest(_T("6.3.5"), realDigest, trust, realBundle);
         verify.timeoutMilliseconds = ChildTimeoutMilliseconds_;
         LoweredChild::Result result;
         expect(LoweredChild::Run(verify, result), "the verifier could not be started lowered: " + Utf8_(result.error));
         std::string identity, time, why;
         expect(AdminUpdate::ReadVerifyAnswer(result.exitCode, result.output, identity, time, why), "the verifier refused the real 6.3.5 bundle: " + why);
         expect(identity.find("refs/tags/v6.3.5") != std::string::npos, "the verifier named " + identity + " for 6.3.5");

         std::vector<unsigned char> flipped = realDigest;
         flipped[0] ^= 0x80;
         verify.input = AdminUpdate::MakeVerifyRequest(_T("6.3.5"), flipped, trust, realBundle);
         expect(LoweredChild::Run(verify, result), "the verifier could not be started lowered");
         expect(result.exitCode == 1 && !AdminUpdate::ReadVerifyAnswer(result.exitCode, result.output, identity, time, why), "the verifier accepted a digest one bit off");

         verify.input = "not a request";
         expect(LoweredChild::Run(verify, result), "the verifier could not be started lowered");
         expect(result.exitCode == 2 && result.output.compare(0, 8, "refused ") == 0, "the verifier did not refuse a malformed request with exit 2");

         verify.input = AdminUpdate::MakeVerifyRequest(_T("6.3.5"), realDigest, trust, realBundle);
         verify.timeoutMilliseconds = 1;
         expect(LoweredChild::Run(verify, result), "the verifier could not be started lowered");
         expect(result.timedOut && !AdminUpdate::ReadVerifyAnswer(result.exitCode, result.output, identity, time, why),
                "a verifier given no time was believed");
      }
   }
}

#endif
