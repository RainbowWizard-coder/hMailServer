// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// hMailServer.Updater: the one-shot helper that applies a verified update.
//
// The service cannot run its own installer - the installer stops the service - so
// the server copies this program out of Bin (which the installer replaces) into the
// data directory's Updates folder and starts it there, detached, with the verified
// installer's path, a rollback image where one could be had, the SHA-256 of each as
// the server checked it, and a single-use token the new installer's database tool
// presents to authenticate the schema upgrade.
//
// Then, alone:
//   1. end whatever is running from under the installation - a Control Panel
//      left open - because the installer replaces those files and, silent, aborts
//      at the first one it cannot; then open the installer so that nothing can
//      change it, require its SHA-256 to be the server's, run it silently by the
//      path Windows resolved that handle to, wait for it, and only then let go;
//   2. wait for the service to be running again, up to --wait seconds;
//   3. if it is not, or the installer says a step after its files did not
//      complete (InstallerStepsIncomplete), run the rollback image - held and
//      held to its SHA-256 the same way, from before the installer runs, which is
//      told of a version to put back only when it is held (RollbackHeld) - and
//      wait again; but never once the
//      installer's database step has changed the database, or could not show it
//      left it unchanged (InstallerDatabaseUpgraded, DatabaseStepOf): the earlier
//      version may not run on that database, and the outcome says so instead
//      (upgraded-incomplete). A rollback is only made onto the database as the
//      earlier version left it;
//   4. write one line to --outcome saying what happened, which the server reads and
//      reports the next time it starts.
//
// No dependencies beyond Win32 and the static C runtime, deliberately: everything
// else on the machine is what is being replaced while this runs.
//
// --mode administrator is the other way it runs: started not by the service but
// by hMailServer.exe /InstallUpdate, elevated, from an administrator-only work
// folder, for an administrator's install on a server whose service cannot run an
// installer. See ADMINISTRATOR MODE below.

#include "stdafx.h"

#include <string>
#include <vector>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <sddl.h>

// Administrator mode's window (the sign-out block), and the desktop's shell window,
// whose process the Control Panel is started again under. user32 is a KnownDLL,
// loaded from System32 like the rest (/DEPENDENTLOADFLAG:0x800).
#pragma comment(lib, "user32.lib")

// SHA-256 from Windows itself (CNG), which is there on every system this runs on.
// bcrypt.dll is NOT imported: this program runs as LocalSystem from the Updates
// folder, and bcrypt.dll is not a KnownDLL, so an import would be looked for
// beside the program first. It is loaded from System32 by name alone
// (Sha256Of), after wmain has limited every load to System32
// (SetDefaultDllDirectories), and the link sets /DEPENDENTLOADFLAG:0x800 for what
// is imported - kernel32 and advapi32, both KnownDLLs, the C runtime being
// static.

namespace
{
   struct Arguments
   {
      std::wstring installer;
      std::wstring installer_sha256;
      std::wstring version;
      std::wstring rollback;
      std::wstring rollback_sha256;
      std::wstring rollback_version;
      std::wstring service;
      std::wstring token;
      std::wstring outcome;
      std::wstring log;
      std::wstring app;
      int wait_seconds;

      // Administrator mode (ADMINISTRATOR MODE, below).
      std::wstring mode;
      std::wstring work;
      std::wstring from;
      std::wstring token_file;
      std::wstring admin_outcome;
      std::wstring in_progress;
      std::wstring relaunch_id;
      std::wstring reader;
      std::wstring wait_process;
      std::wstring cancel_event;
      std::wstring busy;

      Arguments() : service(L"hMailServer"), wait_seconds(180) {}
   };

   std::wstring g_logPath;

   std::wstring Now()
   {
      SYSTEMTIME t;
      GetLocalTime(&t);
      wchar_t buffer[32];
      swprintf_s(buffer, L"%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      return buffer;
   }

   void Log(const std::wstring &line)
   {
      std::wstring text = Now() + L"  " + line + L"\r\n";
      wprintf(L"%s", text.c_str());
      if (g_logPath.empty())
         return;
      HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
         return;
      int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0, nullptr, nullptr);
      std::vector<char> utf8((size_t) needed);
      WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), utf8.data(), needed, nullptr, nullptr);
      DWORD written = 0;
      WriteFile(file, utf8.data(), (DWORD) utf8.size(), &written, nullptr);
      CloseHandle(file);
   }

   bool Parse(int argc, wchar_t *argv[], Arguments &arguments)
   {
      for (int i = 1; i + 1 < argc; i += 2)
      {
         std::wstring name = argv[i];
         std::wstring value = argv[i + 1];
         if (name == L"--installer") arguments.installer = value;
         else if (name == L"--installer-sha256") arguments.installer_sha256 = value;
         else if (name == L"--version") arguments.version = value;
         else if (name == L"--rollback") arguments.rollback = value;
         else if (name == L"--rollback-sha256") arguments.rollback_sha256 = value;
         else if (name == L"--rollback-version") arguments.rollback_version = value;
         else if (name == L"--service") arguments.service = value;
         else if (name == L"--token") arguments.token = value;
         else if (name == L"--outcome") arguments.outcome = value;
         else if (name == L"--log") arguments.log = value;
         else if (name == L"--app") arguments.app = value;
         else if (name == L"--wait") arguments.wait_seconds = _wtoi(value.c_str());
         else if (name == L"--mode") arguments.mode = value;
         else if (name == L"--work") arguments.work = value;
         else if (name == L"--from") arguments.from = value;
         else if (name == L"--token-file") arguments.token_file = value;
         else if (name == L"--admin-outcome") arguments.admin_outcome = value;
         else if (name == L"--in-progress") arguments.in_progress = value;
         else if (name == L"--relaunch-id") arguments.relaunch_id = value;
         else if (name == L"--reader") arguments.reader = value;
         else if (name == L"--wait-process") arguments.wait_process = value;
         else if (name == L"--cancel-event") arguments.cancel_event = value;
         else if (name == L"--busy") arguments.busy = value;
         else return false;
      }
      if (!arguments.mode.empty() && arguments.mode != L"administrator")
         return false;
      return !arguments.installer.empty() && !arguments.outcome.empty();
   }

   DWORD ServiceState(const std::wstring &service)
   {
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!manager)
         return 0;
      DWORD state = 0;
      SC_HANDLE handle = OpenServiceW(manager, service.c_str(), SERVICE_QUERY_STATUS);
      if (handle)
      {
         SERVICE_STATUS_PROCESS status;
         DWORD needed = 0;
         if (QueryServiceStatusEx(handle, SC_STATUS_PROCESS_INFO, (LPBYTE) &status, sizeof(status), &needed))
            state = status.dwCurrentState;
         CloseServiceHandle(handle);
      }
      CloseServiceHandle(manager);
      return state;
   }

   const wchar_t *StateName(DWORD state)
   {
      switch (state)
      {
      case SERVICE_RUNNING: return L"running";
      case SERVICE_STOPPED: return L"stopped";
      case SERVICE_START_PENDING: return L"starting";
      case SERVICE_STOP_PENDING: return L"stopping";
      case SERVICE_PAUSED: return L"paused";
      case 0: return L"not installed";
      default: return L"in transition";
      }
   }

   bool WaitForRunning(const std::wstring &service, int seconds)
   {
      for (int elapsed = 0; elapsed <= seconds; elapsed += 2)
      {
         if (ServiceState(service) == SERVICE_RUNNING)
            return true;
         Sleep(2000);
      }
      return ServiceState(service) == SERVICE_RUNNING;
   }

   // The installer's exit code when its files are in place but a step after them -
   // the service account's folders, the administrator password, the database or
   // the service - did not complete (GetCustomSetupExitCode in
   // hMailServerInnoExtension.iss; Inno Setup's own codes are 1 to 8). The service
   // is often running then, since the installer starts it for the database step,
   // with a server in it that refuses the database: so this is rolled back as a
   // service that did not come back is. Before 6.3.6 the installer exited 0 there,
   // and an update whose database upgrade failed was reported as installed. The
   // installer gives 10 only while the database is as the earlier version left it:
   // it stops before its database step when a step before it did not complete and
   // it has been told there is a version to put back (HM_UPDATE_ROLLBACK_VERSION),
   // and a database step that failed without changing the database's version
   // leaves it so.
   const int InstallerStepsIncomplete = 10;

   // The installer's exit code when a step did not complete after its database
   // step had changed the database, or could not show that it left it unchanged:
   // the database is the new version's, or part-way there, and the earlier
   // version may refuse it. Never rolled back - a rollback is only ever made onto
   // the database as the earlier version left it - and reported as
   // upgraded-incomplete (GetCustomSetupExitCode in hMailServerInnoExtension.iss).
   const int InstallerDatabaseUpgraded = 11;

   std::wstring StepsIncompleteReason(const std::wstring &service)
   {
      return L"the installer exited with " + std::to_wstring(InstallerStepsIncomplete) +
             L": a step after its files (the service account's folders, the administrator password, the database or the service) did not complete; the service is " +
             StateName(ServiceState(service));
   }

   // Runs an installer silently and returns its exit code; -1 when it could not be
   // started, -2 when it ran for longer than an hour.
   int RunInstaller(const std::wstring &installer, const std::wstring &token, const std::wstring &installLog)
   {
      // The log names the token without writing it, as the administrator's path
      // does (RunInstallerIn): the line is the command but for that.
      std::wstring command = L"\"" + installer + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
      std::wstring logged = command;
      if (!token.empty())
      {
         command += L" /upgradetoken=" + token;
         logged += L" /upgradetoken=(the token)";
      }
      if (!installLog.empty())
      {
         command += L" /LOG=\"" + installLog + L"\"";
         logged += L" /LOG=\"" + installLog + L"\"";
      }

      Log(L"Running: " + logged);

      std::vector<wchar_t> mutableCommand(command.begin(), command.end());
      mutableCommand.push_back(L'\0');

      STARTUPINFOW startup;
      memset(&startup, 0, sizeof(startup));
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process;
      memset(&process, 0, sizeof(process));

      if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
      {
         wchar_t message[64];
         swprintf_s(message, L"CreateProcess failed with %lu", GetLastError());
         Log(message);
         return -1;
      }

      DWORD waited = WaitForSingleObject(process.hProcess, 60 * 60 * 1000);
      DWORD exitCode = 0;
      if (waited == WAIT_OBJECT_0)
         GetExitCodeProcess(process.hProcess, &exitCode);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);

      if (waited != WAIT_OBJECT_0)
      {
         Log(L"The installer did not finish within an hour.");
         return -2;
      }

      wchar_t message[64];
      swprintf_s(message, L"The installer exited with %lu", exitCode);
      Log(message);
      return (int) exitCode;
   }

   void WriteOutcome(const std::wstring &path, const std::wstring &line)
   {
      Log(L"Outcome: " + line);
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
         return;
      int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int) line.size(), nullptr, 0, nullptr, nullptr);
      std::vector<char> utf8((size_t) needed);
      WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int) line.size(), utf8.data(), needed, nullptr, nullptr);
      DWORD written = 0;
      WriteFile(file, utf8.data(), (DWORD) utf8.size(), &written, nullptr);
      CloseHandle(file);
   }

   std::wstring Directory(const std::wstring &path)
   {
      size_t slash = path.find_last_of(L"\\/");
      return slash == std::wstring::npos ? L"." : path.substr(0, slash);
   }

   // Whether a release ("6.3.7", "6.3.7-rc1") is at least major.minor.patch.
   bool VersionAtLeast(const std::wstring &version, int major, int minor, int patch)
   {
      int parts[3] = { 0, 0, 0 };
      int part = 0;
      for (size_t i = 0; i < version.size() && part < 3; i++)
      {
         const wchar_t c = version[i];
         if (c >= L'0' && c <= L'9')
            parts[part] = parts[part] * 10 + (c - L'0');
         else if (c == L'.')
            part++;
         else
            break;
      }
      if (parts[0] != major)
         return parts[0] > major;
      if (parts[1] != minor)
         return parts[1] > minor;
      return parts[2] >= patch;
   }

   // The SHA-256 of what an open file holds, read through that handle from its
   // first byte, as lowercase hex; empty when it could not be read.
   std::wstring Sha256Of(HANDLE file)
   {
      std::wstring hex;

      // System32's bcrypt.dll and nothing else: LOAD_LIBRARY_SEARCH_SYSTEM32
      // searches there alone, for the DLL and for what it loads.
      HMODULE cng = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (cng == nullptr)
         return hex;
      decltype(&BCryptOpenAlgorithmProvider) openProvider = (decltype(&BCryptOpenAlgorithmProvider)) GetProcAddress(cng, "BCryptOpenAlgorithmProvider");
      decltype(&BCryptCloseAlgorithmProvider) closeProvider = (decltype(&BCryptCloseAlgorithmProvider)) GetProcAddress(cng, "BCryptCloseAlgorithmProvider");
      decltype(&BCryptCreateHash) createHash = (decltype(&BCryptCreateHash)) GetProcAddress(cng, "BCryptCreateHash");
      decltype(&BCryptHashData) hashData = (decltype(&BCryptHashData)) GetProcAddress(cng, "BCryptHashData");
      decltype(&BCryptFinishHash) finishHash = (decltype(&BCryptFinishHash)) GetProcAddress(cng, "BCryptFinishHash");
      decltype(&BCryptDestroyHash) destroyHash = (decltype(&BCryptDestroyHash)) GetProcAddress(cng, "BCryptDestroyHash");
      BCRYPT_ALG_HANDLE algorithm = nullptr;
      if (!openProvider || !closeProvider || !createHash || !hashData || !finishHash || !destroyHash ||
          !BCRYPT_SUCCESS(openProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
      {
         FreeLibrary(cng);
         return hex;
      }

      BCRYPT_HASH_HANDLE hash = nullptr;
      if (BCRYPT_SUCCESS(createHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
      {
         LARGE_INTEGER start;
         start.QuadPart = 0;
         bool ok = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != FALSE;
         std::vector<unsigned char> buffer(1024 * 1024);
         while (ok)
         {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), (DWORD) buffer.size(), &got, nullptr))
               ok = false;
            else if (got == 0)
               break;
            else if (!BCRYPT_SUCCESS(hashData(hash, buffer.data(), got, 0)))
               ok = false;
         }

         unsigned char digest[32];
         if (ok && BCRYPT_SUCCESS(finishHash(hash, digest, (ULONG) sizeof(digest), 0)))
         {
            const wchar_t *digits = L"0123456789abcdef";
            for (size_t i = 0; i < sizeof(digest); i++)
            {
               hex += digits[digest[i] >> 4];
               hex += digits[digest[i] & 0x0F];
            }
         }
         destroyHash(hash);
      }
      closeProvider(algorithm, 0);
      FreeLibrary(cng);
      return hex;
   }

   // A file this program is about to run, held open from before its SHA-256 is
   // read until the program has finished: shared for reading and nothing else, so
   // nothing can write it, rename it or delete it meanwhile, nor rename a folder
   // above it. It is started by path, the one Windows resolved the handle to, so
   // no link on the way to it can be pointed elsewhere after the check either.
   struct HeldFile
   {
      HANDLE handle;
      std::wstring path;

      HeldFile() : handle(INVALID_HANDLE_VALUE) {}
      ~HeldFile()
      {
         Release();
      }

      // Lets go of the file before the end of the scope: a file held is held
      // without FILE_SHARE_DELETE, so that nothing can delete it, this process
      // included.
      void Release()
      {
         if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
         handle = INVALID_HANDLE_VALUE;
      }

   private:
      HeldFile(const HeldFile &);
      HeldFile &operator=(const HeldFile &);
   };

   // Opens file into held and requires its SHA-256 to be expected, the digest the
   // server read when it checked it; false with why, and nothing to run, when it
   // is not - or when the server gave none, since then nothing says what it is.
   bool Hold(const std::wstring &file, const std::wstring &expected, HeldFile &held, std::wstring &why)
   {
      if (expected.size() != 64)
      {
         why = L"the server gave no SHA-256 for " + file + L", so nothing says it is the file the server checked";
         return false;
      }

      held.handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (held.handle == INVALID_HANDLE_VALUE)
      {
         why = file + L" could not be opened (Windows error " + std::to_wstring(GetLastError()) + L")";
         return false;
      }

      const std::wstring actual = Sha256Of(held.handle);
      if (actual.empty())
      {
         why = L"the SHA-256 of " + file + L" could not be read";
         return false;
      }
      if (_wcsicmp(actual.c_str(), expected.c_str()) != 0)
      {
         why = file + L" is not the file the server checked: its SHA-256 is " + actual + L", not " + expected;
         return false;
      }

      wchar_t buffer[1024];
      const DWORD length = GetFinalPathNameByHandleW(held.handle, buffer, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (length == 0 || length >= 1024)
      {
         why = L"Windows could not say where " + file + L" is (error " + std::to_wstring(GetLastError()) + L")";
         return false;
      }
      std::wstring resolved(buffer, length);
      if (resolved.compare(0, 8, L"\\\\?\\UNC\\") == 0)
         resolved = L"\\\\" + resolved.substr(8);
      else if (resolved.compare(0, 4, L"\\\\?\\") == 0)
         resolved = resolved.substr(4);
      held.path = resolved;
      return true;
   }
}

namespace
{
   // The installation, {app}: what --app names, or else the directory above the
   // service's own binary (...\Bin\hMailServer.exe), read from the service.
   std::wstring InstallationRoot(const Arguments &arguments)
   {
      if (!arguments.app.empty())
         return arguments.app;
      std::wstring root;
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!manager)
         return root;
      SC_HANDLE handle = OpenServiceW(manager, arguments.service.c_str(), SERVICE_QUERY_CONFIG);
      if (handle)
      {
         DWORD needed = 0;
         QueryServiceConfigW(handle, nullptr, 0, &needed);
         if (needed)
         {
            std::vector<BYTE> buffer(needed);
            QUERY_SERVICE_CONFIGW *config = (QUERY_SERVICE_CONFIGW *) buffer.data();
            if (QueryServiceConfigW(handle, config, needed, &needed) && config->lpBinaryPathName)
            {
               // "C:\...\Bin\hMailServer.exe" RunAsService: the quoted path, or the
               // first word when it is not quoted.
               std::wstring binary = config->lpBinaryPathName;
               if (!binary.empty() && binary[0] == L'"')
               {
                  size_t close = binary.find(L'"', 1);
                  binary = close == std::wstring::npos ? binary.substr(1) : binary.substr(1, close - 1);
               }
               else
               {
                  size_t space = binary.find(L' ');
                  if (space != std::wstring::npos)
                     binary = binary.substr(0, space);
               }
               root = Directory(Directory(binary));
            }
         }
         CloseServiceHandle(handle);
      }
      CloseServiceHandle(manager);
      return root;
   }

   DWORD ServiceProcessId(const std::wstring &service)
   {
      DWORD processId = 0;
      SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!manager)
         return 0;
      SC_HANDLE handle = OpenServiceW(manager, service.c_str(), SERVICE_QUERY_STATUS);
      if (handle)
      {
         SERVICE_STATUS_PROCESS status;
         DWORD needed = 0;
         if (QueryServiceStatusEx(handle, SC_STATUS_PROCESS_INFO, (LPBYTE) &status, sizeof(status), &needed))
            processId = status.dwProcessId;
         CloseServiceHandle(handle);
      }
      CloseServiceHandle(manager);
      return processId;
   }

   bool Under(const std::wstring &path, const std::wstring &root)
   {
      if (root.empty() || path.size() <= root.size())
         return false;
      if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0)
         return false;
      wchar_t next = path[root.size()];
      return next == L'\\' || next == L'/';
   }

   // Every program running from under the installation is ended before the
   // installer runs - the Control Panel above all. The installer replaces those
   // files and, silent, aborts at the first one it cannot replace: on 14 September
   // 2026 a Control Panel left open in the operator's session, which the Restart
   // Manager called from session 0 could not close, failed an install and its
   // rollback alike with exit code 5. The service itself is left to the installer,
   // which stops it in order, with its drain period; this program, which runs from
   // the data directory under the installation, is itself.
   int EndProcessesUnder(const std::wstring &root, const std::wstring &service)
   {
      if (root.empty())
         return 0;
      const DWORD self = GetCurrentProcessId();
      const DWORD serviceProcess = ServiceProcessId(service);
      HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snapshot == INVALID_HANDLE_VALUE)
         return 0;
      int ended = 0;
      PROCESSENTRY32W entry;
      entry.dwSize = sizeof(entry);
      if (Process32FirstW(snapshot, &entry))
      {
         do
         {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == self || entry.th32ProcessID == serviceProcess)
               continue;
            if (_wcsicmp(entry.szExeFile, L"hMailServer.exe") == 0)
               continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (!process)
               continue;
            wchar_t image[2 * MAX_PATH];
            DWORD length = 2 * MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, image, &length) && Under(image, root))
            {
               const std::wstring name = entry.szExeFile;
               const std::wstring id = std::to_wstring(entry.th32ProcessID);
               if (TerminateProcess(process, 1))
               {
                  WaitForSingleObject(process, 10000);
                  ended++;
                  Log(L"Ended " + name + L" (process " + id + L"), running from " + image +
                      L": the installer replaces those files, and cannot while they are in use.");
               }
               else
               {
                  Log(L"Could not end " + name + L" (process " + id + L"), running from " + image +
                      L": Windows error " + std::to_wstring(GetLastError()) + L"; the installer may fail on its files.");
               }
            }
            CloseHandle(process);
         } while (Process32NextW(snapshot, &entry));
      }
      CloseHandle(snapshot);
      return ended;
   }

   std::wstring Decoded(const std::vector<char> &bytes)
   {
      if (bytes.size() >= 2 && (BYTE) bytes[0] == 0xFF && (BYTE) bytes[1] == 0xFE)
         return std::wstring((const wchar_t *) (bytes.data() + 2), (bytes.size() - 2) / 2);
      size_t skip = bytes.size() >= 3 && (BYTE) bytes[0] == 0xEF && (BYTE) bytes[1] == 0xBB && (BYTE) bytes[2] == 0xBF ? 3 : 0;
      const char *text = bytes.data() + skip;
      const int size = (int) (bytes.size() - skip);
      if (size <= 0)
         return L"";
      UINT page = CP_UTF8;
      int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, size, nullptr, 0);
      if (!needed)
      {
         page = CP_ACP;
         needed = MultiByteToWideChar(page, 0, text, size, nullptr, 0);
      }
      std::wstring out((size_t) needed, L'\0');
      if (needed)
         MultiByteToWideChar(page, 0, text, size, &out[0], needed);
      return out;
   }

   // What the installer's own log says went wrong, for the outcome, so that the
   // status page says why rather than only where to look: the last few lines that
   // name an error, and the file they were about. Inno Setup writes the log with
   // a timestamp on each line, in UTF-8, or in the system code page from older
   // versions; both are read.
   std::wstring InstallerLogSays(const std::wstring &path)
   {
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
         return L"";
      LARGE_INTEGER size;
      size.QuadPart = 0;
      GetFileSizeEx(file, &size);
      const LONGLONG most = 512 * 1024;
      if (size.QuadPart > most)
      {
         LARGE_INTEGER from;
         from.QuadPart = size.QuadPart - most;
         SetFilePointerEx(file, from, nullptr, FILE_BEGIN);
         size.QuadPart = most;
      }
      std::vector<char> bytes((size_t) size.QuadPart);
      DWORD readCount = 0;
      if (!bytes.empty())
         ReadFile(file, bytes.data(), (DWORD) bytes.size(), &readCount, nullptr);
      CloseHandle(file);
      bytes.resize(readCount);
      const std::wstring text = Decoded(bytes);
      std::vector<std::wstring> said;
      std::wstring about;
      std::wstring subject;
      size_t start = 0;
      while (start < text.size())
      {
         size_t end = text.find(L'\n', start);
         if (end == std::wstring::npos)
            end = text.size();
         std::wstring line = text.substr(start, end - start);
         start = end + 1;
         while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
         // "2026-09-14 01:38:20.123   text": the stamp goes.
         if (line.size() > 24 && iswdigit(line[0]) && line[4] == L'-' && line[10] == L' ')
            line = line.substr(23);
         while (!line.empty() && line.front() == L' ')
            line.erase(line.begin());
         // A message box's text keeps its own breaks inside the line Inno Setup
         // writes - "(exit code 2).\r\rThe hMailServer service ..." in the smoke
         // VM's install-6.3.10.log of 2 October 2026, which put two raw carriage
         // returns into the one-line outcome - so each control character is a
         // space here, and a run of spaces one.
         std::wstring flat;
         for (wchar_t c : line)
         {
            const wchar_t put = (c < 0x20 || c == 0x7F) ? L' ' : c;
            if (!(put == L' ' && !flat.empty() && flat.back() == L' '))
               flat.push_back(put);
         }
         while (!flat.empty() && flat.back() == L' ')
            flat.pop_back();
         line = flat;
         if (line.empty())
            continue;
         std::wstring lower = line;
         for (size_t i = 0; i < lower.size(); i++)
            lower[i] = towlower(lower[i]);
         if (lower.find(L"dest filename:") == 0)
         {
            about = line;
            continue;
         }
         // The files are all in place: an error after this is about a step, not
         // about the last file copied (on the smoke VM every outcome of a failed
         // step began "Dest filename: ...\Bin\hMailServer.INI").
         if (lower.find(L"installation process succeeded.") == 0)
         {
            about.clear();
            continue;
         }
         if (lower.find(L"error") == std::wstring::npos && lower.find(L"abort") == std::wstring::npos && lower.find(L"failed") == std::wstring::npos &&
             lower.find(L"cannot") == std::wstring::npos && lower.find(L"in use") == std::wstring::npos && lower.find(L"denied") == std::wstring::npos &&
             lower.find(L"could not") == std::wstring::npos && lower.find(L"did not complete") == std::wstring::npos &&
             lower.find(L"did not start") == std::wstring::npos)
            continue;
         // Lines that name an error without being one: Inno Setup's record of the
         // functions a script imports (WSAGetLastError), and the service account
         // grant's summary when nothing failed. On the installer smoke VM on 2
         // October 2026 these filled the outcome of an update whose database
         // upgrade had failed, and the line saying so was not among them.
         if (lower.find(L"function and dll name:") == 0 || (lower.find(L"done:") == 0 && lower.find(L" 0 failed") != std::wstring::npos))
            continue;
         // Nor Inno Setup's "Runtime error (at 171:732):", which only says where the
         // script raised the exception whose message follows it.
         if (lower.find(L"runtime error (at ") == 0)
            continue;
         // The file the first error was about is kept apart, so that it is
         // never one of the lines trimmed below.
         if (subject.empty() && !about.empty())
            subject = about;
         said.push_back(line);
      }
      if (said.empty())
         return L"";
      if (said.size() > 4)
         said.erase(said.begin(), said.end() - 4);
      std::wstring out = subject;
      for (size_t i = 0; i < said.size(); i++)
         out += (out.empty() ? L"" : L" | ") + said[i];
      if (out.size() > 600)
         out = out.substr(0, 600) + L"...";
      return L"; the installer's log says: " + out;
   }

   // What the installer's log says of its database step (BeginDatabaseStep,
   // EndDatabaseStep and SayDatabaseStepStoppedPartWay in
   // hMailServerInnoExtension.iss, 6.3.6 and later): whether it began on an
   // existing database, whether it then left the database unchanged, whether it
   // stopped part-way, and the line that says how the database's version moved.
   struct DatabaseStep
   {
      bool begun = false;
      bool unchanged = false;
      bool partWay = false;
      std::wstring changed;

      // Whether the earlier version may not run on the database the installer left:
      // its database step began and did not say it left the database unchanged.
      // An installer before 6.3.6 writes none of this, and nothing is taken to
      // have changed.
      bool MayHaveChanged() const { return begun && !unchanged; }
   };

   DatabaseStep DatabaseStepOf(const std::wstring &path)
   {
      DatabaseStep step;
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
         return step;
      LARGE_INTEGER size;
      size.QuadPart = 0;
      GetFileSizeEx(file, &size);
      // The lines come after every file the installer copied, which is most of a
      // log of a few megabytes; the last 16 MB hold them in any log Inno Setup writes.
      const LONGLONG most = 16 * 1024 * 1024;
      if (size.QuadPart > most)
      {
         LARGE_INTEGER from;
         from.QuadPart = size.QuadPart - most;
         SetFilePointerEx(file, from, nullptr, FILE_BEGIN);
         size.QuadPart = most;
      }
      std::vector<char> bytes((size_t) size.QuadPart);
      DWORD readCount = 0;
      if (!bytes.empty())
         ReadFile(file, bytes.data(), (DWORD) bytes.size(), &readCount, nullptr);
      CloseHandle(file);
      bytes.resize(readCount);
      const std::wstring text = Decoded(bytes);
      size_t start = 0;
      while (start < text.size())
      {
         size_t end = text.find(L'\n', start);
         if (end == std::wstring::npos)
            end = text.size();
         std::wstring line = text.substr(start, end - start);
         start = end + 1;
         while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
         // Only a line of its own in the log, "2026-10-02 12:23:04.328   Database
         // step ...", as the installer's Log() writes each of these: never a line of
         // what a program the installer ran said, which a multi-line Log() puts in
         // the log without the stamp (the grant's report of every folder, the
         // message box texts) and which could begin with anything.
         if (!(line.size() > 24 && iswdigit(line[0]) && line[4] == L'-' && line[10] == L' '))
            continue;
         line = line.substr(23);
         while (!line.empty() && line.front() == L' ')
            line.erase(line.begin());
         // The prefix as a std::wstring, its length its own: no wcslen over a pointer.
         auto starts = [&line](const std::wstring &prefix) { return line.compare(0, prefix.size(), prefix) == 0; };
         // What follows the step's beginning is what counts: a line before it
         // says nothing of it.
         if (starts(L"Database step begun:"))
         {
            step = DatabaseStep();
            step.begun = true;
         }
         else if (!step.begun)
            continue;
         else if (starts(L"Database step left the database unchanged"))
            step.unchanged = true;
         else if (starts(L"Database step stopped part-way"))
            step.partWay = true;
         else if (starts(L"Database step changed the database") || starts(L"Database step may have changed the database"))
            step.changed = line.size() > 400 ? line.substr(0, 400) : line;
      }
      while (!step.changed.empty() && step.changed.back() == L'.')
         step.changed.pop_back();
      return step;
   }

   // Where the backup the update took before it is, as the service wrote it into
   // the Updates folder (update-backup-<version>.txt: the backup destination, then
   // the time the backup finished, written by UpdateInstaller for this install alone
   // and removed when it took none). Text for the outcome and nothing else: never a
   // path anything here opens. Empty when there is no such file, or it is not one
   // line or two of printable text, or it is a link - a reparse point, or a file
   // with a second name (a hard link): the folder is the service's to write, the
   // administrator's install reads this elevated, and what it reads goes into an
   // outcome the service may read, so a hard link there to a file only
   // administrators may read would hand the service its first lines.
   std::wstring BackupNote(const std::wstring &updatesFolder, const std::wstring &version)
   {
      const std::wstring path = updatesFolder + L"\\update-backup-" + version + L".txt";
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
         return std::wstring();
      FILE_ATTRIBUTE_TAG_INFO tag;
      memset(&tag, 0, sizeof(tag));
      FILE_STANDARD_INFO standard;
      memset(&standard, 0, sizeof(standard));
      LARGE_INTEGER size;
      size.QuadPart = 0;
      std::vector<char> bytes;
      if (GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) && (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
          GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) && standard.NumberOfLinks == 1 &&
          GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 2048)
      {
         bytes.resize((size_t) size.QuadPart);
         DWORD got = 0;
         if (!ReadFile(file, bytes.data(), (DWORD) bytes.size(), &got, nullptr))
            got = 0;
         bytes.resize(got);
      }
      CloseHandle(file);
      std::wstring text = Decoded(bytes);
      std::vector<std::wstring> lines;
      size_t begin = 0;
      while (begin < text.size() && lines.size() < 3)
      {
         size_t end = text.find(L'\n', begin);
         if (end == std::wstring::npos)
            end = text.size();
         std::wstring line = text.substr(begin, end - begin);
         begin = end + 1;
         while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
            line.pop_back();
         if (!line.empty())
            lines.push_back(line);
      }
      if (lines.empty() || lines.size() > 2)
         return std::wstring();
      for (const std::wstring &line : lines)
      {
         for (wchar_t c : line)
         {
            if (c < 0x20 || c == 0x7F)
               return std::wstring();
         }
      }
      return lines.size() == 2 ? lines[0] + L" (finished " + lines[1] + L")" : lines[0];
   }

   // An update whose database step changed the database, or may have, and that then
   // did not complete - exit code 11, a service that did not stay up after exit
   // code 0, an installer that never finished: not rolled back, and the outcome says
   // what did not complete, how the database moved, and what to do - finish the
   // upgrade, or put the earlier version back with the backup the update took.
   std::wstring UpgradedIncompleteDetail(int code, int waitSeconds, const std::wstring &service, const DatabaseStep &step, const std::wstring &says,
                                         const std::wstring &previous, const std::wstring &backup)
   {
      std::wstring text = code == 0
         ? L"the installer succeeded but the service was not running after " + std::to_wstring(waitSeconds) + L" seconds"
         : code == InstallerDatabaseUpgraded ? L"the installer exited with " + std::to_wstring(InstallerDatabaseUpgraded) + L": a step did not complete after its database step had changed the database"
         : code == -2 ? std::wstring(L"the installer did not finish within an hour, and its database step had begun")
         : L"the installer exited with " + std::to_wstring(code) + L" after its database step had begun";
      text += L"; " + (!step.changed.empty() ? step.changed
         : step.begun ? std::wstring(L"its log says the database step began, and not that it left the database unchanged")
         : std::wstring(L"the database step changed the database, or could not show it left it unchanged"));
      text += L"; " + previous + L" was not put back, since it may not run on that database; the service is " + StateName(ServiceState(service)) + says;
      const std::wstring where = backup.empty()
         ? std::wstring(L"the backup taken before the update, if one was taken (UpdateBackupBeforeApply) - the newest HMBackup archive in the backup destination Backup, restore & migration names -")
         : L"the backup taken before the update, in " + backup + L",";
      if (step.partWay)
         text += L"; the database upgrade stopped part-way: once its cause is put right, run DBSetupQuick.exe in the Bin folder to finish it, or restore " + where + L" and reinstall " + previous;
      else
         text += L"; put right what did not complete and start the service, or, to go back to " + previous + L", restore " + where + L" and reinstall it";
      return text;
   }

   // The rollback image, held and held to its SHA-256 BEFORE the installer runs,
   // and kept held - so that nothing can change or remove it - until it has run or
   // is not needed. Only an installer whose rollback is in hand is told of a
   // version to put back (HM_UPDATE_ROLLBACK_VERSION): told so, a 6.3.6 or later
   // installer stops before its database step when a step before it did not
   // complete, which leaves the new version's program on the earlier version's
   // database, the service not started - a server again only once the rollback
   // has run. Where the image cannot be held (gone, changed, unreadable), the
   // installer is told nothing and carries on past such a step as it does when run
   // by hand; the rollback, if one is needed, then fails as it always did, with why.
   bool RollbackHeld(const Arguments &arguments, HeldFile &held, std::wstring &why)
   {
      if (arguments.rollback.empty())
         return false;
      if (Hold(arguments.rollback, arguments.rollback_sha256, held, why))
      {
         Log(L"The rollback image is held until it has run or is not needed: " + held.path);
         return true;
      }
      held.Release();
      Log(L"The rollback image could not be held before the installer runs: " + why +
          L". The installer is not told of a version to put back, and carries on past a step that does not complete, as it does when run by hand.");
      return false;
   }

   // Whether the service is running once the installer has finished. After exit
   // code 10 or 11 the installer has said what happened, and what is done next -
   // the rollback on 10, nothing on 11 - does not depend on the service: so its
   // state now, without the wait for it to come back, which after an installer that
   // stopped before its database step - one that does not start the service - only
   // kept the server down for the whole wait before the rollback began (three
   // minutes on the installer smoke VM on 2 October 2026). Otherwise up to --wait.
   bool ServiceCameBack(int code, const Arguments &arguments)
   {
      if (code == InstallerStepsIncomplete || code == InstallerDatabaseUpgraded)
         return ServiceState(arguments.service) == SERVICE_RUNNING;
      return WaitForRunning(arguments.service, arguments.wait_seconds);
   }
}

// ---------------------------------------------------------------------------
// ADMINISTRATOR MODE: an administrator's install of an update
// (docs/ServiceAccount.md, "The live update"; Common/Util/AdminUpdate.h).
//
// On a server whose service cannot run an installer, the Control Panel on the
// server has asked Windows to elevate hMailServer.exe /InstallUpdate, which
// copied the installer into {app}\AdminUpdate\<id> - a folder only
// administrators can write - checked it there, and started this copy of the
// Updater from the same folder, with the administrator's elevated token. It
// passed three handles and nothing else: the Control Panel's process, to wait on;
// the cancel event the Control Panel may set; and the busy file, held
// exclusively so that no second install starts meanwhile.
//
// Here, in order: nothing happens until the Control Panel has closed with the
// exit code that means go (anything else, or the cancel event, is a
// cancellation); then the token the service issued for the database upgrade is
// read, the programs running from {app} are ended, sign-out and shutdown are
// held off, and the installer is run - held by the SHA-256 the stage read - with
// the unattended apply's switches, its temporary files in the administrators'
// folder and the token in its environment, never on its command line. If the
// service does not come back, or the installer says a step after its files did
// not complete, the rollback image is run, held the same way - unless the
// installer's database step changed the database, as in the unattended apply.
// The outcome goes into administrator-only files the service may read; an event
// in the Application log names the administrator; and the Control Panel is
// started again, not elevated, as the user of the desktop.
//
// Nothing is written where the service can write: the log, the outcome and the
// installer's temporary files are all in the work folder or in AdminUpdate.
// ---------------------------------------------------------------------------

namespace
{
   const DWORD GoExitCode = 0x484D5550;   // "HMUP"; AdminUpdate::GoExitCode, Services/AdministratorUpdate.cs
   const DWORD ControlPanelWaitMilliseconds = 15 * 60 * 1000;

   struct AdminArguments
   {
      std::wstring work;
      std::wstring app;
      std::wstring from;
      std::wstring tokenFile;
      std::wstring adminOutcome;
      std::wstring inProgress;
      std::wstring relaunchId;
      std::wstring reader;
      HANDLE waitProcess;
      HANDLE cancelEvent;
      HANDLE busy;

      AdminArguments() : waitProcess(nullptr), cancelEvent(nullptr), busy(nullptr) {}
   };

   bool IsLowerHex(const std::wstring &text, size_t length)
   {
      if (text.size() != length)
         return false;
      for (wchar_t c : text)
      {
         if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')))
            return false;
      }
      return true;
   }

   bool IsVersionText(const std::wstring &text)
   {
      if (text.empty() || text.size() > 40)
         return false;
      for (wchar_t c : text)
      {
         if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.' || c == L'-'))
            return false;
      }
      return true;
   }

   HANDLE HandleOf(const std::wstring &text)
   {
      if (text.empty() || text.size() > 20)
         return nullptr;
      unsigned long long value = 0;
      for (wchar_t c : text)
      {
         if (c < L'0' || c > L'9')
            return nullptr;
         value = value * 10 + (unsigned long long) (c - L'0');
      }
      return (HANDLE) (uintptr_t) value;
   }

   std::wstring WithoutPrefix(const std::wstring &path)
   {
      if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
         return L"\\\\" + path.substr(8);
      if (path.compare(0, 4, L"\\\\?\\") == 0)
         return path.substr(4);
      return path;
   }

   bool SamePath(const std::wstring &a, const std::wstring &b)
   {
      std::wstring left = WithoutPrefix(a), right = WithoutPrefix(b);
      while (left.size() > 3 && left.back() == L'\\')
         left.pop_back();
      while (right.size() > 3 && right.back() == L'\\')
         right.pop_back();
      return CompareStringOrdinal(left.c_str(), (int) left.size(), right.c_str(), (int) right.size(), TRUE) == CSTR_EQUAL;
   }

   std::wstring FinalPathOf(HANDLE handle)
   {
      std::vector<wchar_t> buffer(32768);
      const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (length == 0 || length >= buffer.size())
         return std::wstring();
      return WithoutPrefix(std::wstring(buffer.data(), length));
   }

   std::wstring ModulePath()
   {
      std::vector<wchar_t> buffer(32768);
      const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD) buffer.size());
      return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
   }

   std::wstring LongPath(const std::wstring &path)
   {
      std::vector<wchar_t> buffer(32768);
      const DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), (DWORD) buffer.size());
      return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : path;
   }

   bool IsElevatedAdministrator()
   {
      BYTE buffer[SECURITY_MAX_SID_SIZE];
      DWORD size = sizeof(buffer);
      BOOL member = FALSE;
      return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, buffer, &size) && CheckTokenMembership(nullptr, buffer, &member) && member;
   }

   // A privilege of this process's token switched on, or off again. Enabled, it is
   // in force for this process and for every process it starts - the installer
   // among them.
   bool SetPrivilege(const wchar_t *name, bool enabled)
   {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
         return false;
      TOKEN_PRIVILEGES privileges;
      privileges.PrivilegeCount = 1;
      privileges.Privileges[0].Attributes = enabled ? SE_PRIVILEGE_ENABLED : 0;
      const bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) &&
                      AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
      CloseHandle(token);
      return ok;
   }

   // The account this process runs as - the administrator who approved the
   // prompt - as DOMAIN\user, and its SID for the event log.
   std::wstring OwnAccount(std::vector<unsigned char> &sid)
   {
      sid.clear();
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
         return L"(unknown)";
      DWORD needed = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
      std::vector<unsigned char> buffer(needed ? needed : 1);
      std::wstring name = L"(unknown)";
      if (GetTokenInformation(token, TokenUser, buffer.data(), (DWORD) buffer.size(), &needed))
      {
         PSID user = ((TOKEN_USER *) buffer.data())->User.Sid;
         sid.assign((unsigned char *) user, (unsigned char *) user + GetLengthSid(user));
         wchar_t account[256], domain[256];
         DWORD accountLength = 256, domainLength = 256;
         SID_NAME_USE use;
         if (LookupAccountSidW(nullptr, user, account, &accountLength, domain, &domainLength, &use))
            name = std::wstring(domain) + L"\\" + account;
      }
      CloseHandle(token);
      return name;
   }

   // The service's own SID, NT SERVICE\<service>, for the Read entry on the two
   // files the service reads at its next start. Empty when Windows does not know it.
   std::wstring ServiceSidText(const std::wstring &service)
   {
      BYTE buffer[SECURITY_MAX_SID_SIZE];
      DWORD size = sizeof(buffer);
      wchar_t domain[256];
      DWORD domainLength = 256;
      SID_NAME_USE use;
      const std::wstring name = L"NT SERVICE\\" + service;
      if (!LookupAccountNameW(nullptr, name.c_str(), buffer, &size, domain, &domainLength, &use))
         return std::wstring();
      LPWSTR text = nullptr;
      if (!ConvertSidToStringSidW((PSID) buffer, &text) || text == nullptr)
         return std::wstring();
      std::wstring result(text);
      LocalFree(text);
      return result;
   }

   bool IsSidText(const std::wstring &text)
   {
      PSID sid = nullptr;
      if (!ConvertStringSidToSidW(text.c_str(), &sid))
         return false;
      LocalFree(sid);
      return text.compare(0, 4, L"S-1-") == 0;
   }

   // A file written new in the administrators' folder: whatever was there removed
   // first (CREATE_ALWAYS would keep the old file's permissions), then made with
   // SYSTEM and the Administrators able to change it and each reader able to
   // read it, and nobody else.
   bool WriteAdminFile(const std::wstring &path, const std::string &utf8, const std::vector<std::wstring> &readers, DWORD readerRights)
   {
      DeleteFileW(path.c_str());
      std::wstring sddl = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
      for (const std::wstring &reader : readers)
      {
         if (reader.empty())
            continue;
         wchar_t rights[16];
         swprintf_s(rights, L"0x%08lx", readerRights);
         sddl += L"(A;;" + std::wstring(rights) + L";;;" + reader + L")";
      }
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
         return false;
      SECURITY_ATTRIBUTES attributes;
      memset(&attributes, 0, sizeof(attributes));
      attributes.nLength = sizeof(attributes);
      attributes.lpSecurityDescriptor = descriptor;
      HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      LocalFree(descriptor);
      if (file == INVALID_HANDLE_VALUE)
         return false;
      DWORD written = 0;
      const BOOL ok = utf8.empty() || WriteFile(file, utf8.data(), (DWORD) utf8.size(), &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
      return ok && written == utf8.size();
   }

   std::string Utf8(const std::wstring &text)
   {
      if (text.empty())
         return std::string();
      const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0, nullptr, nullptr);
      std::string out((size_t) needed, '\0');
      WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), &out[0], needed, nullptr, nullptr);
      return out;
   }

   std::wstring UtcNow()
   {
      SYSTEMTIME t;
      GetSystemTime(&t);
      wchar_t text[32];
      swprintf_s(text, L"%04d-%02d-%02dT%02d:%02d:%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
      return text;
   }

   // The token UpdateApplyToken issued, from the file the service wrote: refused
   // when the name leads through a link, is not a regular file, or does not hold
   // exactly 64 lowercase hexadecimal characters. The service could rewrite it -
   // the file is its own - and that is not a way in: the token only lets the new
   // installer's database upgrade sign in as the administrator, which the
   // service holds already, and a wrong one fails the upgrade and so rolls back.
   bool ReadToken(const std::wstring &path, std::string &token, std::wstring &why)
   {
      token.clear();
      HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (file == INVALID_HANDLE_VALUE)
      {
         why = L"the token file " + path + L" could not be opened (Windows error " + std::to_wstring(GetLastError()) + L")";
         return false;
      }
      FILE_ATTRIBUTE_TAG_INFO tag;
      memset(&tag, 0, sizeof(tag));
      LARGE_INTEGER size;
      size.QuadPart = 0;
      bool ok = GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 && GetFileSizeEx(file, &size) && size.QuadPart == 64 &&
                SamePath(FinalPathOf(file), LongPath(path));
      if (ok)
      {
         char buffer[64];
         DWORD got = 0;
         ok = ReadFile(file, buffer, 64, &got, nullptr) && got == 64;
         if (ok)
            token.assign(buffer, 64);
      }
      CloseHandle(file);
      if (ok)
      {
         for (char c : token)
            ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
      }
      if (!ok)
      {
         SecureZeroMemory(&token[0], token.size());
         token.clear();
         why = L"the token file " + path + L" is not a token: a link, not 64 hexadecimal characters, or not where it was named";
      }
      return ok;
   }

   // The environment the installer runs with: this process's, less
   // HM_ADMIN_PASSWORD, any HM_UPGRADE_TOKEN and any HM_UPDATE_ROLLBACK_VERSION,
   // with TEMP and TMP in the work folder - where only administrators can write, so
   // that nothing can plant a DLL beside the files the installer unpacks - the
   // token, when there is one to pass this way, and the version this program puts
   // back if the installer does not complete, when it holds that version's
   // installer (HM_UPDATE_ROLLBACK_VERSION): a 6.3.6 or later installer then stops
   // before its database step when a step before it did not complete, so that the
   // rollback meets the database as that version left it. Never for the rollback
   // image itself, which has nothing to go back to. Without a temp (the service's
   // own apply, which runs as LocalSystem with SYSTEM's TEMP) TEMP and TMP are this
   // process's.
   std::vector<wchar_t> InstallerEnvironment(const std::wstring &temp, const std::string &token, const std::wstring &rollbackVersion = std::wstring())
   {
      std::vector<wchar_t> block;
      LPWCH current = GetEnvironmentStringsW();
      for (LPWCH entry = current; entry != nullptr && *entry != L'\0'; entry += wcslen(entry) + 1)
      {
         const std::wstring line = entry;
         std::wstring name = line.substr(0, line.find(L'=', 1));
         for (wchar_t &c : name)
            c = (wchar_t) towupper(c);
         if (name == L"HM_ADMIN_PASSWORD" || name == L"HM_UPGRADE_TOKEN" || name == L"HM_UPDATE_ROLLBACK_VERSION" || (!temp.empty() && (name == L"TEMP" || name == L"TMP")))
            continue;
         block.insert(block.end(), line.begin(), line.end());
         block.push_back(L'\0');
      }
      if (current != nullptr)
         FreeEnvironmentStringsW(current);

      std::vector<std::wstring> added;
      if (!temp.empty())
      {
         added.push_back(L"TEMP=" + temp);
         added.push_back(L"TMP=" + temp);
      }
      if (!token.empty())
         added.push_back(L"HM_UPGRADE_TOKEN=" + std::wstring(token.begin(), token.end()));
      if (!rollbackVersion.empty())
         added.push_back(L"HM_UPDATE_ROLLBACK_VERSION=" + rollbackVersion);
      for (const std::wstring &line : added)
      {
         block.insert(block.end(), line.begin(), line.end());
         block.push_back(L'\0');
      }
      block.push_back(L'\0');
      return block;
   }

   // As RunInstaller, with the environment given and, for an installer older than
   // 6.3.6 - which reads no HM_UPGRADE_TOKEN - the token on its command line.
   int RunInstallerIn(const std::wstring &installer, const std::string &commandLineToken, std::vector<wchar_t> &environment, const std::wstring &installLog,
                      const std::wstring &directory)
   {
      std::wstring command = L"\"" + installer + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
      if (!installLog.empty())
         command += L" /LOG=\"" + installLog + L"\"";
      Log(L"Running: " + command + (commandLineToken.empty() ? L"" : L" /upgradetoken=(the token)"));
      if (!commandLineToken.empty())
         command += L" /upgradetoken=" + std::wstring(commandLineToken.begin(), commandLineToken.end());

      std::vector<wchar_t> mutableCommand(command.begin(), command.end());
      mutableCommand.push_back(L'\0');
      STARTUPINFOW startup;
      memset(&startup, 0, sizeof(startup));
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process;
      memset(&process, 0, sizeof(process));
      const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                          environment.data(), directory.c_str(), &startup, &process);
      SecureZeroMemory(mutableCommand.data(), mutableCommand.size() * sizeof(wchar_t));
      if (!started)
      {
         Log(L"CreateProcess failed with " + std::to_wstring(GetLastError()));
         return -1;
      }
      const DWORD waited = WaitForSingleObject(process.hProcess, 60 * 60 * 1000);
      DWORD exitCode = 0;
      if (waited == WAIT_OBJECT_0)
         GetExitCodeProcess(process.hProcess, &exitCode);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      if (waited != WAIT_OBJECT_0)
      {
         Log(L"The installer did not finish within an hour.");
         return -2;
      }
      Log(L"The installer exited with " + std::to_wstring(exitCode));
      return (int) exitCode;
   }

   // Sign-out and shutdown held off while the installer runs: a sign-out would
   // end this process and the installer with it, and leave the server stopped.
   // A window of this process's own, on a thread of its own that answers the
   // query, as ShutdownBlockReasonCreate requires.
   class ShutdownBlock
   {
   public:
      ShutdownBlock() : window_(nullptr), thread_(nullptr), ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
      {
         thread_ = CreateThread(nullptr, 0, &ShutdownBlock::Run, this, 0, nullptr);
         if (thread_ && ready_)
            WaitForSingleObject(ready_, 10000);
      }

      // Closes its thread and event and posts to its window when it goes, and
      // the thread holds this object's address: a copy would close them twice.
      ShutdownBlock(const ShutdownBlock &) = delete;
      ShutdownBlock &operator=(const ShutdownBlock &) = delete;

      ~ShutdownBlock()
      {
         if (window_)
            PostMessageW(window_, WM_CLOSE, 0, 0);
         if (thread_)
         {
            WaitForSingleObject(thread_, 10000);
            CloseHandle(thread_);
         }
         if (ready_)
            CloseHandle(ready_);
      }

   private:
      static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l)
      {
         switch (message)
         {
         case WM_QUERYENDSESSION:
            return FALSE;
         case WM_CLOSE:
            ShutdownBlockReasonDestroy(window);
            DestroyWindow(window);
            return 0;
         case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
         default:
            return DefWindowProcW(window, message, w, l);
         }
      }

      static DWORD WINAPI Run(LPVOID parameter)
      {
         ShutdownBlock *self = (ShutdownBlock *) parameter;
         WNDCLASSW type;
         memset(&type, 0, sizeof(type));
         type.lpfnWndProc = &ShutdownBlock::Procedure;
         type.hInstance = GetModuleHandleW(nullptr);
         type.lpszClassName = L"hMailServerUpdaterShutdownBlock";
         RegisterClassW(&type);
         self->window_ = CreateWindowExW(0, type.lpszClassName, L"hMailServer update", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
         if (self->window_)
         {
            if (ShutdownBlockReasonCreate(self->window_, L"hMailServer is being updated. Signing out now would stop it half way."))
               Log(L"Sign-out and shutdown are held off while the installer runs.");
         }
         SetEvent(self->ready_);
         MSG message;
         while (self->window_ && GetMessageW(&message, nullptr, 0, 0) > 0)
         {
            TranslateMessage(&message);
            DispatchMessageW(&message);
         }
         return 0;
      }

      HWND window_;
      HANDLE thread_;
      HANDLE ready_;
   };

   // The user a process runs as, as a SID's text; empty when it cannot be read.
   std::wstring ProcessUserSid(HANDLE process)
   {
      std::wstring user;
      HANDLE token = nullptr;
      if (!OpenProcessToken(process, TOKEN_QUERY, &token))
         return user;
      DWORD needed = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
      std::vector<unsigned char> buffer(needed ? needed : 1);
      LPWSTR text = nullptr;
      if (GetTokenInformation(token, TokenUser, buffer.data(), (DWORD) buffer.size(), &needed) &&
          ConvertSidToStringSidW(((TOKEN_USER *) buffer.data())->User.Sid, &text) && text != nullptr)
      {
         user = text;
         LocalFree(text);
      }
      CloseHandle(token);
      return user;
   }

   // The Control Panel started again as the user of the desktop, not elevated: as
   // a child of the desktop's own shell - the process that owns this desktop's
   // shell window - named to CreateProcess as its parent
   // (PROC_THREAD_ATTRIBUTE_PARENT_PROCESS), so that it runs with the shell's
   // token: the desktop user's own, unelevated, at medium integrity, in the
   // shell's session and job, with the shell as its parent. The shell must be
   // Windows' explorer.exe, in this process's session, run by the user who
   // clicked Install update (readerSid, whose Control Panel started the stage).
   // Nothing of this process goes to the child: no handle is inherited, and the
   // environment is this process's, which Windows' elevation took from that
   // Control Panel. No shell - nobody signed in to this desktop - starts nothing,
   // and the outcome waits for the next start of the Control Panel.
   //
   // Asking the shell to start it instead, through the desktop's
   // IShellDispatch2::ShellExecute found by IShellWindows::FindWindowSW, needs the
   // desktop to be among the shell's windows, and it is not always: on the
   // installer smoke VM (Windows Server 2025) on 1 October 2026, with the first
   // sign-in's privacy screen still up, FindWindowSW answered S_FALSE, elevated or
   // not, and no Control Panel came back, while GetShellWindow named Explorer.
   // The security a process the shell starts itself is given: owned by the shell's
   // user, with the shell's token's default DACL (that user and SYSTEM in full, the
   // logon session to read) and the shell's integrity as its mandatory label (no
   // write or read up). A process this elevated program creates is otherwise given
   // this program's own - owned by the Administrators, full control to them and
   // SYSTEM alone, labelled high - whatever its token: on the installer smoke VM on
   // 2 October 2026 the Control Panel opened again so, though it ran at medium,
   // could not be ended by the user it ran as from anything unelevated (Access is
   // denied). Read fills the buffers that attributes points into, so the object
   // must outlive its use.
   struct ShellSecurity
   {
      std::vector<unsigned char> user, dacl, label, sacl;
      SECURITY_DESCRIPTOR descriptor;
      SECURITY_ATTRIBUTES attributes;

      ShellSecurity()
      {
         memset(&descriptor, 0, sizeof(descriptor));
         memset(&attributes, 0, sizeof(attributes));
      }

      static bool Query(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<unsigned char> &buffer)
      {
         DWORD needed = 0;
         GetTokenInformation(token, kind, nullptr, 0, &needed);
         if (needed == 0)
            return false;
         buffer.assign(needed, 0);
         return GetTokenInformation(token, kind, buffer.data(), needed, &needed) != FALSE;
      }

      bool Read(HANDLE shell)
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(shell, TOKEN_QUERY, &token))
            return false;
         const bool ok = Query(token, TokenUser, user) && Query(token, TokenDefaultDacl, dacl) && Query(token, TokenIntegrityLevel, label);
         CloseHandle(token);
         if (!ok)
            return false;
         PSID owner = ((const TOKEN_USER *) user.data())->User.Sid;
         PACL defaultDacl = ((const TOKEN_DEFAULT_DACL *) dacl.data())->DefaultDacl;
         PSID integrity = ((const TOKEN_MANDATORY_LABEL *) label.data())->Label.Sid;
         if (defaultDacl == nullptr || integrity == nullptr)
            return false;
         const DWORD saclSize = (DWORD) (sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + GetLengthSid(integrity));
         sacl.assign(saclSize, 0);
         PACL labelAcl = (PACL) sacl.data();
         if (!InitializeAcl(labelAcl, saclSize, ACL_REVISION) ||
             !AddMandatoryAce(labelAcl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP, integrity) ||
             !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorOwner(&descriptor, owner, FALSE) ||
             !SetSecurityDescriptorDacl(&descriptor, TRUE, defaultDacl, FALSE) || !SetSecurityDescriptorSacl(&descriptor, TRUE, labelAcl, FALSE))
            return false;
         attributes.nLength = sizeof(attributes);
         attributes.lpSecurityDescriptor = &descriptor;
         attributes.bInheritHandle = FALSE;
         return true;
      }
   };

   bool RelaunchAsDesktopUser(const std::wstring &program, const std::wstring &arguments, const std::wstring &directory, const std::wstring &readerSid,
                              std::wstring &why)
   {
      const HWND shellWindow = GetShellWindow();
      DWORD shellId = 0;
      if (shellWindow == nullptr || GetWindowThreadProcessId(shellWindow, &shellId) == 0 || shellId == 0)
      {
         why = L"this desktop has no shell window; nobody may be signed in to it";
         return false;
      }
      HANDLE shell = OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellId);
      if (shell == nullptr)
      {
         why = L"the desktop's shell, process " + std::to_wstring(shellId) + L", could not be opened (Windows error " + std::to_wstring(GetLastError()) + L")";
         return false;
      }

      std::vector<wchar_t> image(32768);
      DWORD imageLength = (DWORD) image.size();
      wchar_t windows[MAX_PATH];
      const UINT windowsLength = GetSystemWindowsDirectoryW(windows, MAX_PATH);
      const std::wstring shellImage = QueryFullProcessImageNameW(shell, 0, image.data(), &imageLength) ? std::wstring(image.data(), imageLength) : std::wstring();
      DWORD ownSession = 0, shellSession = 0;
      const std::wstring shellUser = ProcessUserSid(shell);
      bool ok = true;
      if (shellImage.empty() || windowsLength == 0 || windowsLength >= MAX_PATH ||
          !SamePath(LongPath(shellImage), LongPath(std::wstring(windows, windowsLength) + L"\\explorer.exe")))
      {
         ok = false;
         why = L"the desktop's shell is " + (shellImage.empty() ? std::wstring(L"a program that could not be named") : shellImage) + L", not Windows' explorer.exe";
      }
      else if (!ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) || !ProcessIdToSessionId(shellId, &shellSession) || ownSession != shellSession)
      {
         ok = false;
         why = L"the desktop's shell is in another session";
      }
      else if (shellUser.empty() || _wcsicmp(shellUser.c_str(), readerSid.c_str()) != 0)
      {
         ok = false;
         why = L"the desktop's shell runs as " + (shellUser.empty() ? std::wstring(L"an account that could not be read") : shellUser) +
               L", not as the user who clicked Install update (" + readerSid + L")";
      }
      if (!ok)
      {
         CloseHandle(shell);
         return false;
      }

      SIZE_T attributeSize = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
      std::vector<unsigned char> attributeBuffer(attributeSize ? attributeSize : 1);
      LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) attributeBuffer.data();
      const bool initialised = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) != FALSE;
      const bool listed = initialised && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell, sizeof(shell), nullptr, nullptr) != FALSE;

      wchar_t desktop[] = L"WinSta0\\Default";
      STARTUPINFOEXW startup;
      memset(&startup, 0, sizeof(startup));
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.lpDesktop = desktop;
      startup.lpAttributeList = attributes;
      PROCESS_INFORMATION process;
      memset(&process, 0, sizeof(process));
      std::wstring command = L"\"" + program + L"\" " + arguments;
      std::vector<wchar_t> line(command.begin(), command.end());
      line.push_back(L'\0');
      ShellSecurity security;
      const bool secured = security.Read(shell);
      if (!secured)
         Log(L"The shell's own default security could not be read (Windows error " + std::to_wstring(GetLastError()) +
             L"); the Control Panel is started with this program's, which only an administrator can end it under.");
      BOOL started = listed ? CreateProcessW(program.c_str(), line.data(), secured ? &security.attributes : nullptr, secured ? &security.attributes : nullptr, FALSE,
                                              EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(), &startup.StartupInfo, &process) : FALSE;
      DWORD error = GetLastError();
      // The shell's user may not be this program's - an administrator's
      // credentials typed into the prompt of a standard user's Control Panel - and
      // then cannot be made the owner (ERROR_INVALID_OWNER): this program's own
      // security then, as before.
      if (listed && !started && secured)
      {
         Log(L"The Control Panel could not be started with the shell's own security (Windows error " + std::to_wstring(error) + L"); it is started with this program's.");
         started = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
                                  &startup.StartupInfo, &process);
         error = GetLastError();
      }
      if (initialised)
         DeleteProcThreadAttributeList(attributes);
      CloseHandle(shell);
      if (!started)
      {
         why = L"it could not be started under the desktop's shell (Windows error " + std::to_wstring(error) + L")";
         return false;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return true;
   }

   // One entry in the Application log, source hMailServer, as the server's own
   // errors are - the id is the HM number, and the user is the administrator.
   void ReportUpdaterEvent(WORD type, DWORD id, const std::wstring &text, const std::vector<unsigned char> &sid)
   {
      HANDLE source = RegisterEventSourceW(nullptr, L"hMailServer");
      if (!source)
         return;
      LPCWSTR strings[1] = { text.c_str() };
      ReportEventW(source, type, 0, id, sid.empty() ? nullptr : (PSID) sid.data(), 1, 0, strings, nullptr);
      DeregisterEventSource(source);
   }

   int RunAdministrator(const Arguments &arguments)
   {
      AdminArguments admin;
      admin.work = arguments.work;
      admin.app = arguments.app;
      admin.from = arguments.from;
      admin.tokenFile = arguments.token_file;
      admin.adminOutcome = arguments.admin_outcome;
      admin.inProgress = arguments.in_progress;
      admin.relaunchId = arguments.relaunch_id;
      admin.reader = arguments.reader;
      admin.waitProcess = HandleOf(arguments.wait_process);
      admin.cancelEvent = HandleOf(arguments.cancel_event);
      admin.busy = HandleOf(arguments.busy);

      const std::wstring outcomePath = arguments.outcome;
      std::vector<unsigned char> ownSid;
      const std::wstring by = OwnAccount(ownSid);
      const std::wstring started = UtcNow();
      std::vector<std::wstring> readers;
      readers.push_back(admin.reader);
      std::vector<std::wstring> serviceReaders = readers;
      const std::wstring serviceSid = ServiceSidText(arguments.service);
      if (!serviceSid.empty())
         serviceReaders.push_back(serviceSid);

      // The rollback image once it is held (4, below), let go of before the outcome
      // is written, which deletes it after a success.
      HeldFile rollbackImage;

      // The outcome, in the work folder for the Control Panel's user, and in
      // AdminUpdate for the service's next start as well; the mark of the install
      // in progress removed; the event; and the Control Panel back.
      auto finish = [&](const std::wstring &status, const std::wstring &detail, const std::wstring &installLog, const std::wstring &rollbackLog,
                        bool relaunch) -> int
      {
         rollbackImage.Release();
         std::wstring text = status + L" " + arguments.version + L" " + detail + L"\r\n";
         text += L"id=" + admin.relaunchId + L"\r\n";
         text += L"by=" + by + L"\r\n";
         text += L"from=" + admin.from + L"\r\n";
         text += L"started=" + started + L"\r\n";
         text += L"finished=" + UtcNow() + L"\r\n";
         if (!installLog.empty())
            text += L"install-log=" + installLog + L"\r\n";
         if (!rollbackLog.empty())
            text += L"rollback-log=" + rollbackLog + L"\r\n";
         if (status != L"ok")
         {
            text += L"installer=" + arguments.installer + L"\r\n";
            if (!arguments.rollback.empty())
               text += L"rollback-image=" + arguments.rollback + L"\r\n";
         }
         Log(L"Outcome: " + status + L" " + arguments.version + L" " + detail);
         WriteAdminFile(outcomePath, Utf8(text), readers, FILE_GENERIC_READ);
         WriteAdminFile(admin.adminOutcome, Utf8(text), serviceReaders, FILE_GENERIC_READ);
         DeleteFileW(admin.inProgress.c_str());

         // As docs/ServiceAccount.md has HM7467: installed, information; cancelled
         // or refused before anything ran, a warning; not installed, rolled back,
         // with its rollback failed, or installed with the database upgraded and a
         // step not complete (upgraded-incomplete), an error.
         const WORD type = status == L"ok" ? EVENTLOG_INFORMATION_TYPE : (status == L"cancelled" || status == L"refused")
            ? EVENTLOG_WARNING_TYPE : EVENTLOG_ERROR_TYPE;
         ReportUpdaterEvent(type, 7467, L"hMailServer: the administrator's install of hMailServer " + arguments.version + L" by " + by + L": " + status + L" - " + detail +
                     L" (work folder " + admin.work + L")", ownSid);

         // The copies of the installers are what the next check would find: on a
         // success nothing needs them, and they are hundreds of megabytes.
         if (status == L"ok")
         {
            DeleteFileW(arguments.installer.c_str());
            DeleteFileW((arguments.installer + L".cosign.bundle").c_str());
            if (!arguments.rollback.empty())
            {
               DeleteFileW(arguments.rollback.c_str());
               DeleteFileW((arguments.rollback + L".cosign.bundle").c_str());
            }
         }

         if (relaunch)
         {
            const std::wstring panel = admin.app + L"\\ControlPanel\\hMailCP.exe";
            std::wstring why;
            if (RelaunchAsDesktopUser(panel, L"--after-update " + admin.relaunchId, admin.app + L"\\ControlPanel", admin.reader, why))
               Log(L"The Control Panel was started again as the desktop's user, under the desktop's shell: " + panel);
            else
               Log(L"The Control Panel was not started again: " + why + L". The outcome is in " + outcomePath + L" and the Application log.");
         }
         return status == L"ok" ? 0 : 1;
      };

      // Where this program is, and what it was given: only a copy in its own work
      // folder under {app}\AdminUpdate, elevated, given paths inside that folder.
      const std::wstring self = LongPath(ModulePath());
      const std::wstring adminUpdate = admin.app + L"\\AdminUpdate";
      const std::wstring workName = admin.work.substr(admin.work.find_last_of(L'\\') + 1);
      const bool placed = IsElevatedAdministrator() && SamePath(Directory(self), admin.work) && SamePath(Directory(admin.work), adminUpdate) &&
                          IsLowerHex(workName, 32) && workName == admin.relaunchId && SamePath(Directory(arguments.installer), admin.work) &&
                          (arguments.rollback.empty() || SamePath(Directory(arguments.rollback), admin.work + L"\\rollback")) &&
                          SamePath(outcomePath, admin.work + L"\\outcome.txt") && SamePath(admin.adminOutcome, adminUpdate + L"\\last-install.txt") &&
                          SamePath(admin.inProgress, adminUpdate + L"\\in-progress") && SamePath(g_logPath, admin.work + L"\\apply.log") &&
                          IsVersionText(arguments.version) && IsVersionText(admin.from) && IsSidText(admin.reader) &&
                          admin.waitProcess != nullptr && admin.cancelEvent != nullptr && admin.busy != nullptr;
      if (!placed)
      {
         Log(L"Refused: this copy of the Updater is not where an administrator's install puts it, or was given something it does not take.");
         return 2;
      }

      // The work folder held, so that it cannot be renamed while this runs.
      HANDLE workHeld = CreateFileW(admin.work.c_str(), FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);

      {
         std::wstring mark = L"id=" + admin.relaunchId + L"\r\nversion=" + arguments.version + L"\r\nfrom=" + admin.from + L"\r\nby=" + by + L"\r\nstarted=" + started + L"\r\n";
         WriteAdminFile(admin.inProgress, Utf8(mark), serviceReaders, FILE_GENERIC_READ);
      }
      Log(L"hMailServer.Updater, administrator mode: " + by + L" installs hMailServer " + arguments.version + L" over " + admin.from + L" (work folder " + admin.work + L")" +
          (arguments.rollback.empty() ? L"; no rollback image" : L"; rollback image " + arguments.rollback));
      ReportUpdaterEvent(EVENTLOG_INFORMATION_TYPE, 7466, L"hMailServer: " + by + L" is installing hMailServer " + arguments.version + L" over " + admin.from +
                  L" (work folder " + admin.work + L").", ownSid);

      // 1. Nothing until the Control Panel has closed with the go code.
      {
         HANDLE waits[2] = { admin.waitProcess, admin.cancelEvent };
         const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, ControlPanelWaitMilliseconds);
         DWORD exitCode = 0;
         if (woke == WAIT_OBJECT_0 && GetExitCodeProcess(admin.waitProcess, &exitCode) && exitCode == GoExitCode)
            Log(L"The Control Panel has closed to let the installer run.");
         else
         {
            const std::wstring why = woke == WAIT_OBJECT_0 + 1 ? L"the install was cancelled from the Control Panel before it started"
               : woke == WAIT_OBJECT_0 ? L"the Control Panel closed without starting the install (exit code " + std::to_wstring(exitCode) + L")"
               : L"the Control Panel did not close to let the installer run within 15 minutes";
            if (workHeld != INVALID_HANDLE_VALUE)
               CloseHandle(workHeld);
            return finish(L"cancelled", why + L"; nothing was installed", L"", L"", false);
         }
      }

      // 2. The token for the database upgrade, which the service issued a moment ago.
      std::string token;
      std::wstring tokenWhy;
      if (!ReadToken(admin.tokenFile, token, tokenWhy))
      {
         if (workHeld != INVALID_HANDLE_VALUE)
            CloseHandle(workHeld);
         return finish(L"failed", L"the installer was not run: " + tokenWhy, L"", L"", true);
      }

      // 3. The programs running from {app} ended - a Control Panel open in
      // another session above all - since the installer replaces their files.
      // SeDebugPrivilege only for that: off again before the installer, which would
      // otherwise start with it enabled.
      SetPrivilege(SE_DEBUG_NAME, true);
      const int ended = EndProcessesUnder(admin.app, arguments.service);
      const bool debugOff = SetPrivilege(SE_DEBUG_NAME, false);
      Log(L"Installation: " + admin.app + L"; " + std::to_wstring(ended) + L" program(s) running from it ended before the installer runs." +
          (debugOff ? L"" : L" SeDebugPrivilege could not be switched off again, so the installer starts with it enabled."));

      ShutdownBlock block;

      // 4. The installer, held by the digest the stage read, run by the path its
      // handle resolves to, with its temporary files in the work folder.
      const std::wstring temp = admin.work + L"\\tmp";
      CreateDirectoryW(temp.c_str(), nullptr);
      const std::wstring installLog = admin.work + L"\\install-" + arguments.version + L".log";

      // The rollback image, held and held to the digest the stage read before the
      // installer runs, and kept so until it has run or is not needed (RollbackHeld).
      std::wstring rollbackWhy;
      const bool rollbackHeld = RollbackHeld(arguments, rollbackImage, rollbackWhy);

      int code = 0;
      {
         HeldFile installer;
         std::wstring heldWhy;
         if (!Hold(arguments.installer, arguments.installer_sha256, installer, heldWhy))
         {
            SecureZeroMemory(&token[0], token.size());
            if (workHeld != INVALID_HANDLE_VALUE)
               CloseHandle(workHeld);
            return finish(L"refused", L"the installer was not run: " + heldWhy, L"", L"", true);
         }
         // 6.3.6 and later read the token from the environment; an older one - a
         // test's, since the stage installs only a release newer than 6.3.6 from
         // 6.3.6 on - only from its command line.
         const bool environmentToken = VersionAtLeast(arguments.version, 6, 3, 6);
         // The version to put back, for the installer to stop before its database
         // step on: only with a rollback image held to run, and only to an
         // installer that reads it (6.3.6 and later, as the token).
         const std::wstring putBack = environmentToken && rollbackHeld && IsVersionText(arguments.rollback_version) ? arguments.rollback_version : std::wstring();
         std::vector<wchar_t> environment = InstallerEnvironment(temp, environmentToken ? token : std::string(), putBack);
         Log(L"The installer is the file the stage checked (SHA-256 " + arguments.installer_sha256 + L"), held open until it has finished; the token is passed in its " +
             (environmentToken ? L"environment." : L"command line (an installer before 6.3.6).") +
             (putBack.empty() ? L"" : L" It is told that hMailServer " + putBack + L" is put back if it does not complete (HM_UPDATE_ROLLBACK_VERSION)."));
         DeleteFileW(installLog.c_str());
         code = RunInstallerIn(installer.path, environmentToken ? std::string() : token, environment, installLog, admin.work);
         SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
      }
      SecureZeroMemory(&token[0], token.size());

      const std::wstring says = code == 0 ? L"" : InstallerLogSays(installLog);
      const bool running = ServiceCameBack(code, arguments);
      const bool incomplete = code == InstallerStepsIncomplete;
      if (code == 0 && running)
      {
         if (workHeld != INVALID_HANDLE_VALUE)
            CloseHandle(workHeld);
         return finish(L"ok", L"the installer succeeded and the service is running", installLog, L"", true);
      }

      // A database the installer's database step changed, or may have: never
      // rolled back, since the earlier version may not run on it.
      const DatabaseStep step = DatabaseStepOf(installLog);
      if (code == InstallerDatabaseUpgraded || step.MayHaveChanged())
      {
         if (workHeld != INVALID_HANDLE_VALUE)
            CloseHandle(workHeld);
         Log(L"Not rolled back: the installer's database step changed the database, or could not show it left it unchanged.");
         return finish(L"upgraded-incomplete",
                       UpgradedIncompleteDetail(code, arguments.wait_seconds, arguments.service, step, says, L"hMailServer " + admin.from,
                                                BackupNote(Directory(admin.tokenFile), arguments.version)),
                       installLog, L"", true);
      }

      if (running && !incomplete)
      {
         if (workHeld != INVALID_HANDLE_VALUE)
            CloseHandle(workHeld);
         return finish(L"failed", L"the installer exited with " + std::to_wstring(code) + L" but the service is running" + says, installLog, L"", true);
      }

      const std::wstring reason = code == 0
         ? L"the installer succeeded but the service was not running after " + std::to_wstring(arguments.wait_seconds) + L" seconds (" + StateName(ServiceState(arguments.service)) + L")"
         : incomplete ? StepsIncompleteReason(arguments.service) + says
         : L"the installer exited with " + std::to_wstring(code) + L" and the service was not running (" + StateName(ServiceState(arguments.service)) + L")" + says;

      if (arguments.rollback.empty())
      {
         if (workHeld != INVALID_HANDLE_VALUE)
            CloseHandle(workHeld);
         return finish(L"no-rollback", reason + L"; no rollback image was available", installLog, L"", true);
      }

      // 5. The rollback image, held since before the installer ran, run without a
      // token as the unattended apply runs it.
      Log(L"Rolling back to hMailServer " + arguments.rollback_version + L": " + reason);
      const std::wstring rollbackLog = admin.work + L"\\rollback-" + arguments.rollback_version + L".log";
      SetPrivilege(SE_DEBUG_NAME, true);
      EndProcessesUnder(admin.app, arguments.service);
      SetPrivilege(SE_DEBUG_NAME, false);
      int rollbackCode = 0;
      {
         if (!rollbackHeld)
         {
            if (workHeld != INVALID_HANDLE_VALUE)
               CloseHandle(workHeld);
            return finish(L"rollback-failed", reason + L"; the rollback image was not run: " + rollbackWhy + L"; the service is " + StateName(ServiceState(arguments.service)),
                          installLog, L"", true);
         }
         std::vector<wchar_t> environment = InstallerEnvironment(temp, std::string());
         rollbackCode = RunInstallerIn(rollbackImage.path, std::string(), environment, rollbackLog, admin.work);
      }
      const std::wstring rollbackSays = rollbackCode == 0 ? L"" : InstallerLogSays(rollbackLog);
      // A rollback image whose own steps did not complete has not put the
      // version back, whatever the service's state.
      const bool rolledBack = rollbackCode != InstallerStepsIncomplete && rollbackCode != InstallerDatabaseUpgraded &&
                              WaitForRunning(arguments.service, arguments.wait_seconds);
      if (workHeld != INVALID_HANDLE_VALUE)
         CloseHandle(workHeld);
      if (rolledBack)
         return finish(L"rolled-back", reason + L"; hMailServer " + arguments.rollback_version + L" was reinstalled and the service is running", installLog, rollbackLog, true);
      return finish(L"rollback-failed", reason + L"; the rollback to " + arguments.rollback_version + L" exited with " + std::to_wstring(rollbackCode) + L" and the service is " +
                    StateName(ServiceState(arguments.service)) + rollbackSays, installLog, rollbackLog, true);
   }
}

int wmain(int argc, wchar_t *argv[])
{
   // Parsing loads nothing. Then, before anything that could: every DLL this
   // process loads from here on - bcrypt.dll for the digests, and whatever a
   // Windows function loads on its behalf - is looked for in System32 alone,
   // never in the Updates folder this runs from. Where that cannot be done,
   // nothing is run: a helper that could load a planted DLL as LocalSystem must
   // not go on.
   Arguments arguments;
   if (!Parse(argc, argv, arguments))
   {
      wprintf(L"Usage: hMailServer.Updater --installer <path> --installer-sha256 <hex> --version <v> --outcome <path>\n"
              L"                           [--rollback <path> --rollback-sha256 <hex> --rollback-version <v>]\n"
              L"                           [--service <name>] [--token <token>] [--log <path>] [--wait <seconds>] [--app <root>]\n"
              L"       administrator mode, as hMailServer.exe /InstallUpdate starts it: --mode administrator and its own switches\n");
      return 2;
   }

   if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
   {
      WriteOutcome(arguments.outcome, L"failed " + arguments.version + L" the update helper could not limit its DLL search to System32 (Windows error " +
         std::to_wstring(GetLastError()) + L"), so nothing was run");
      return 3;
   }

   g_logPath = arguments.log;

   // An administrator's install: its own course, below; the rest of this function
   // is the service's unattended apply, unchanged.
   if (arguments.mode == L"administrator")
      return RunAdministrator(arguments);

   Log(L"hMailServer.Updater: applying hMailServer " + arguments.version + L" from " + arguments.installer +
       (arguments.rollback.empty() ? L"; no rollback image" : L"; rollback image " + arguments.rollback + L" (" + arguments.rollback_version + L")"));
   Log(L"Service " + arguments.service + L" is " + StateName(ServiceState(arguments.service)) + L" before the installer runs.");

   // The service that started this process is about to be stopped by the installer.
   // A moment for it to finish returning from the call that started us.
   Sleep(2000);

   std::wstring installLog = Directory(arguments.outcome) + L"\\install-" + arguments.version + L".log";
   const std::wstring root = InstallationRoot(arguments);
   if (root.empty())
      Log(L"The installation's directory is not known, so nothing running from it can be ended before the installer runs.");
   else
      Log(L"Installation: " + root + L"; " + std::to_wstring(EndProcessesUnder(root, arguments.service)) + L" program(s) running from it ended before the installer runs.");

   // The rollback image, held and held to the server's digest before the installer
   // runs, and kept so until it has run or is not needed (RollbackHeld).
   HeldFile rollbackImage;
   std::wstring rollbackWhy;
   const bool rollbackHeld = RollbackHeld(arguments, rollbackImage, rollbackWhy);

   // The installer as the server checked it, or nothing: held from here until it
   // has finished, so that what runs is what has the server's digest. The service
   // is still running when this refuses - the installer is what stops it - so
   // the server reads the outcome and leaves the drain itself.
   int code = 0;
   {
      HeldFile installer;
      std::wstring heldWhy;
      if (!Hold(arguments.installer, arguments.installer_sha256, installer, heldWhy))
      {
         WriteOutcome(arguments.outcome, L"failed " + arguments.version + L" the installer was not run: " + heldWhy);
         return 1;
      }
      // A 6.3.6 or later installer reads the token from its environment, as the
      // administrator's install gives it; on its command line it would be in the
      // installer's own log, which writes the command line it was started with
      // ("Setup command line: ..."). An older one reads only its command line.
      //
      // An earlier installer's log by the same name goes first: what this one's says
      // of its database step decides whether anything is put back (DatabaseStepOf).
      DeleteFileW(installLog.c_str());
      if (VersionAtLeast(arguments.version, 6, 3, 6))
      {
         std::string token;
         for (wchar_t c : arguments.token)
            token.push_back((char) c);
         // The version this puts back if the installer does not complete, which
         // stops the installer before its database step when a step before it
         // did not complete (InstallerEnvironment): only with the rollback image
         // held to run.
         const std::wstring putBack = rollbackHeld && IsVersionText(arguments.rollback_version) ? arguments.rollback_version : std::wstring();
         std::vector<wchar_t> environment = InstallerEnvironment(std::wstring(), token, putBack);
         Log(L"The installer is the file the server checked (SHA-256 " + arguments.installer_sha256 + L"), held open until it has finished" +
             (token.empty() ? L"." : L"; the token is passed in its environment.") +
             (putBack.empty() ? L"" : L" It is told that hMailServer " + putBack + L" is put back if it does not complete (HM_UPDATE_ROLLBACK_VERSION)."));
         code = RunInstallerIn(installer.path, std::string(), environment, installLog, Directory(installLog));
         SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
         if (!token.empty())
            SecureZeroMemory(&token[0], token.size());
      }
      else
      {
         Log(L"The installer is the file the server checked (SHA-256 " + arguments.installer_sha256 + L"), held open until it has finished; the token is passed on its command line (an installer before 6.3.6).");
         code = RunInstaller(installer.path, arguments.token, installLog);
      }
   }
   const std::wstring says = code == 0 ? L"" : InstallerLogSays(installLog);
   bool running = ServiceCameBack(code, arguments);
   const bool incomplete = code == InstallerStepsIncomplete;

   wchar_t codeText[32];
   swprintf_s(codeText, L"%d", code);

   if (code == 0 && running)
   {
      WriteOutcome(arguments.outcome, L"ok " + arguments.version + L" the installer succeeded and the service is running");
      return 0;
   }

   // A database the installer's database step changed, or may have: never rolled
   // back, since the earlier version may not run on it.
   const DatabaseStep step = DatabaseStepOf(installLog);
   if (code == InstallerDatabaseUpgraded || step.MayHaveChanged())
   {
      Log(L"Not rolled back: the installer's database step changed the database, or could not show it left it unchanged.");
      WriteOutcome(arguments.outcome, L"upgraded-incomplete " + arguments.version + L" " +
         UpgradedIncompleteDetail(code, arguments.wait_seconds, arguments.service, step, says,
                                  arguments.rollback_version.empty() ? std::wstring(L"the earlier version") : L"hMailServer " + arguments.rollback_version,
                                  BackupNote(Directory(arguments.outcome), arguments.version)) +
         L"; see " + installLog);
      return 1;
   }

   if (running && !incomplete)
   {
      WriteOutcome(arguments.outcome, L"failed " + arguments.version + L" the installer exited with " + codeText +
         L" but the service is running" + says + L"; see " + installLog);
      return 1;
   }

   std::wstring reason = code == 0
      ? L"the installer succeeded but the service was not running after " + std::to_wstring(arguments.wait_seconds) + L" seconds (" + StateName(ServiceState(arguments.service)) + L")"
      : incomplete ? StepsIncompleteReason(arguments.service) + says
      : L"the installer exited with " + std::wstring(codeText) + L" and the service was not running (" + StateName(ServiceState(arguments.service)) + L")" + says;

   if (arguments.rollback.empty())
   {
      WriteOutcome(arguments.outcome, L"no-rollback " + arguments.version + L" " + reason + L"; no rollback image was available; see " + installLog);
      return 1;
   }

   Log(L"Rolling back to hMailServer " + arguments.rollback_version + L": " + reason);
   std::wstring rollbackLog = Directory(arguments.outcome) + L"\\rollback-" + arguments.rollback_version + L".log";
   if (!root.empty())
      EndProcessesUnder(root, arguments.service);
   // The rollback image, held since before the installer ran (RollbackHeld).
   if (!rollbackHeld)
   {
      WriteOutcome(arguments.outcome, L"rollback-failed " + arguments.version + L" " + reason + L"; the rollback image was not run: " + rollbackWhy +
         L"; the service is " + StateName(ServiceState(arguments.service)) + L"; see " + installLog);
      return 1;
   }
   const int rollbackCode = RunInstaller(rollbackImage.path, L"", rollbackLog);
   const std::wstring rollbackSays = rollbackCode == 0 ? L"" : InstallerLogSays(rollbackLog);
   bool rolledBack = rollbackCode != InstallerStepsIncomplete && rollbackCode != InstallerDatabaseUpgraded && WaitForRunning(arguments.service, arguments.wait_seconds);

   wchar_t rollbackText[32];
   swprintf_s(rollbackText, L"%d", rollbackCode);

   if (rolledBack)
   {
      WriteOutcome(arguments.outcome, L"rolled-back " + arguments.version + L" " + reason + L"; hMailServer " + arguments.rollback_version +
         L" was reinstalled and the service is running; see " + installLog);
      return 1;
   }

   WriteOutcome(arguments.outcome, L"rollback-failed " + arguments.version + L" " + reason + L"; the rollback to " + arguments.rollback_version +
      L" exited with " + rollbackText + L" and the service is " + StateName(ServiceState(arguments.service)) + rollbackSays +
      L"; see " + installLog + L" and " + rollbackLog);
   return 1;
}
