// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "stdafx.h"
// START: Enable tracking of memory leaks.

#ifdef _DEBUG
   #define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
   #define new DEBUG_NEW
#endif

#include "resource.h"
#include "hMailServer.h"

#include "../Common/Util/ServiceManager.h"
#include "../Common/Util/ServiceAccount.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Application/WindowsEventLog.h"
// For the verbs that read hMailServer.ini; /Register reads nothing from it since
// 6.3.6 - neither the account (below) nor where the error log is, which no verb
// writes (ForbidServiceFolderWrites).
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/EnvironmentSettings.h"
#include "../Common/Application/NewInstallDefaults.h"
#include "../Common/Application/Profiles.h"
#include "../Common/Application/WindowsEventLog.h"
#include "../Common/Persistence/MessageStoreMaintenance.h"
#include "../Common/Persistence/MessageStoreObjects.h"
#include "../Common/Util/ClassTester.h"
#include "../Common/Util/SystemInformation.h"
#include "../Common/Util/CrashOracle.h"
#include "../Common/Persistence/SchemaUpgrader.h"
#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/DALConnectionFactory.h"
#include "../Common/SQL/DatabaseSettings.h"
#include "../Common/SQL/DatabaseTls.h"
#include "../Common/Util/Crypt.h"
#include "../Common/Util/StoredSecrets.h"
#include "../Common/Util/ProtectedFile.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Pst/PstReaderChild.h"
#include "../Common/Util/AdminUpdate.h"
#include "../Common/Util/AutoBanFirewall.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
#include <shellapi.h>

// CommandLineToArgvW, for the switches that take a folder, which may have
// spaces in it.
#pragma comment(lib, "shell32.lib")

// #define VLD_START_DISABLED
// #include "C:\Temp\vld-10\vldapi.h"
// #include "C:\Temp\vld-10\vld.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

using namespace HM;

// Declarations of some extremely global 
// functions and variables.
void InitializeApplication();
static void WINAPI ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv);
static void WINAPI ServiceController (DWORD Opcode);
void TerminateServer();
#define SERVICE_SUBMIT_MAIL 200
#define SERVICE_RELOAD_CONFIGURATION 201

SERVICE_STATUS          ServiceStatus; 
SERVICE_STATUS_HANDLE   ServiceStatusHandle; 

DWORD WINAPI StartServiceInitialization(LPVOID vd);

bool DEBUG_MODE = false;

// The service was started with --apply-new-install-defaults (ServiceMain): this
// start finishes a new installation its administrator could not
// (FinishNewInstallationAtStart).
static bool g_finishNewInstallationAtStart = false;

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   class ChMailServerModule : public CAtlExeModuleT< ChMailServerModule >
   {
   public :
      DECLARE_LIBID(LIBID_hMailServer)
      DECLARE_REGISTRY_APPID_RESOURCEID(IDR_HMAILSERVER, "{5EDEC473-39E0-43F6-A234-1947071721C8}")
      
      HRESULT PreMessageLoop(int nShowCmd) throw()
      //---------------------------------------------------------------------------//
      // This function is called by __super. We are only supposed to do the
      // initialization in it. But since we're really free to do whatever we
      // want, we simply start the service and wait for that to terminate.
      // When the service has terminated, we return S_FALSE, which will lead to
      // __super to quit.
      //---------------------------------------------------------------------------//
      {
         if (run_as_service_)
         {
            // Start the server threads now.
            StartServiceInitialization(0);
            return S_FALSE;
         }

         return S_OK;
      }

      void
      RegisterObjects()
      {
         HRESULT hr = CoInitializeSecurity(0, -1, 0, 0, RPC_C_AUTHN_LEVEL_CONNECT , RPC_C_IMP_LEVEL_IMPERSONATE, 0,0,0);

         if (FAILED(hr))
         {
            String sErrorMessage;
            sErrorMessage.Format(_T("CoInitializeSecurity returned %d"), hr);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5078, "hMailServer::RegisterObjects", sErrorMessage);

            return;
         }

         hr = RegisterClassObjects(CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED);

         if (FAILED(hr))
         {
            String sErrorMessage;
            sErrorMessage.Format(_T("RegisterClassObjects returned %d"), hr);
            
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5079, "hMailServer::RegisterObjects", sErrorMessage);

            return;
         }
      }

      void
      ResumeObjects()
      {
         HRESULT hr = CoResumeClassObjects();

         if (FAILED(hr))
         {
            String sErrorMessage;
            sErrorMessage.Format(_T("CoResumeClassObjects returned %d"), hr);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5080, "hMailServer::ResumeObjects", sErrorMessage);

            return;
         }

      }

      LONG Unlock() throw()
      //---------------------------------------------------------------------------//
      // This function is called when an client releases it's reference to
      // hMailServer. If this function returns 0, we are deleted. When running
      // as service, we should never die using this mechanism, hence the return 1.
      //---------------------------------------------------------------------------//
      {
         if (run_as_service_)
            return 1;
         else
            return __super::Unlock();
      }

      HRESULT RegisterAppID()
      //------------------------------------------------------------------------//
      // This function registers an APP-ID in the registry. If this isn't registered
      // it wont be possible to use CreateObject("hMailServer.Application") to get
      // a reference to the running application. 
      //------------------------------------------------------------------------//
      {
         CRegKey keyAppID;
         LONG lRes = keyAppID.Open(HKEY_CLASSES_ROOT, _T("AppID"), KEY_WRITE);
         if (lRes != ERROR_SUCCESS)
            return AtlHresultFromWin32(lRes);

         CRegKey key;

         lRes = key.Create(keyAppID, GetAppIdT());
         if (lRes != ERROR_SUCCESS)
            return AtlHresultFromWin32(lRes);

         key.DeleteValue(_T("LocalService"));
         if (lRes != ERROR_SUCCESS)
            return AtlHresultFromWin32(lRes);

         key.SetStringValue(_T("LocalService"), _T("hMailServer"));
         if (lRes != ERROR_SUCCESS)
            return AtlHresultFromWin32(lRes);

         return S_OK;
      }

      void UnregisterLocalService()
      //------------------------------------------------------------------------//
      // Takes the LocalService value off the AppID for the life of this process,
      // so that a client's CreateObject("hMailServer.Application") reaches the
      // class objects this process registers instead of asking the service
      // control manager to start the service. RegisterAppID puts the value back
      // on the next start of any kind but /Unregister, so nothing is left behind.
      //------------------------------------------------------------------------//
      {
         CRegKey keyAppID;
         if (keyAppID.Open(HKEY_CLASSES_ROOT, _T("AppID"), KEY_WRITE) != ERROR_SUCCESS)
            return;

         CRegKey key;
         if (key.Open(keyAppID, GetAppIdT(), KEY_WRITE) != ERROR_SUCCESS)
            return;

         key.DeleteValue(_T("LocalService"));
      }

      bool run_as_service_;
   };

   
}

HM::ChMailServerModule _AtlModule;

// hMailServer.exe --upgrade-database: the upgrade chain from the command line,
// as `hmailserver --upgrade-database` runs it on Linux (platform/main.cpp) and
// through the same unit, SchemaUpgrader - for an administrator who copied new
// binaries in, or a deployment script, without DBUpdater's window and without
// the administrator password DBUpdater asks for. ADO is here, so all four
// backends. The process is a Windows-subsystem one with no console of its own:
// the parent's is attached when there is one, so a prompt sees the lines, and
// the exit code (0 done or nothing to do, 1 failed, 2 not configured, 3 the
// service is running) says the rest to a script that has no console either.
// Whether the hMailServer service is anything but stopped: asked of the service
// controller, not assumed. A service that is not installed, or that cannot be
// queried, is not running.
static bool HMailServerServiceIsRunning()
{
   bool running = false;
   SC_HANDLE manager = OpenSCManager(NULL, NULL, GENERIC_READ);
   if (manager != NULL)
   {
      SC_HANDLE service = OpenService(manager, _T("hMailServer"), SERVICE_QUERY_STATUS);
      if (service != NULL)
      {
         SERVICE_STATUS status;
         memset(&status, 0, sizeof(status));
         if (QueryServiceStatus(service, &status))
            running = status.dwCurrentState != SERVICE_STOPPED;
         CloseServiceHandle(service);
      }
      CloseServiceHandle(manager);
   }
   return running;
}

// The command lines that run the server itself and write its logs: RunAsService,
// the service controller's (ServiceManager::RegisterService), and /Debug and
// /Test, a developer's console server and ClassTester, which run as whoever
// starts them. The mode is the last token of the trimmed line, as _tWinMain
// reads it. Every other command line is a verb an administrator, Setup, DBSetup
// or the Control Panel runs - elevated, since each reads hMailServer.INI's
// [Database] and [Security] or changes the service, its database or its folders
// - and writes nothing where the service can write (ForbidServiceFolderWrites).
// build/check-elevated-verbs.py holds this list to these three.
static bool IsServerCommandLine()
{
   HM::String commandLine = GetCommandLine();
   commandLine.Trim();
   std::vector<String> tokens = StringParser::SplitString(commandLine, " ");
   String last;
   for (auto token = tokens.rbegin(); token != tokens.rend(); ++token)
   {
      if (!token->IsEmpty())
      {
         last = *token;
         break;
      }
   }

   return last.CompareNoCase(_T("RunAsService")) == 0 ||
          last.CompareNoCase(_T("/Debug")) == 0 ||
          last.CompareNoCase(_T("/Test")) == 0;
}

// What a verb an administrator runs does before anything else, as the live
// update's two verbs do (AdminUpdate.h): no log line, no error, no event script
// and no Application log event from this process, ever (Logger::
// ForbidWritesForThisProcess) - the log folder is the service's to write,
// Modify for NT SERVICE\hMailServer, so a line written there by an elevated
// process could be carried by a link the service made to a file it may not
// write itself (docs/ServiceAccount.md, "What an administrator's command
// writes"). Each verb says what it did and why it failed on its own output
// instead. And, where Windows has it (Windows 11 22H2, Windows Server 2025),
// the process follows no junction a user who is not an administrator made
// (ProcessRedirectionTrustPolicy), so that a write the verb means to make in
// the data folder - the certificate of a new installation, a message rewritten
// by a store walk - cannot be carried elsewhere either; on an older Windows the
// verbs that write there check for links themselves. The crash oracle is not
// installed for these command lines: its record and its dump go to the log
// folder.
static void ForbidServiceFolderWrites()
{
   HM::Logger::ForbidWritesForThisProcess();

   // PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY, value 16, declared here for
   // the SDKs that lack it; bit 0 is EnforceRedirectionTrust. An older Windows
   // refuses the call, which changes nothing.
   struct RedirectionTrustPolicy
   {
      DWORD flags;
   };
   RedirectionTrustPolicy policy = { 1 };
   SetProcessMitigationPolicy((PROCESS_MITIGATION_POLICY) 16, &policy, sizeof(policy));
}

// The lines of a verb go to the handles the caller gave - a script's or a test's
// redirected output - or, when there are none, to the console the caller has.
static void AttachVerbOutput()
{
   if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL && AttachConsole(ATTACH_PARENT_PROCESS))
   {
      HANDLE console = CreateFile(_T("CONOUT$"), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
      if (console != INVALID_HANDLE_VALUE)
      {
         SetStdHandle(STD_OUTPUT_HANDLE, console);
         SetStdHandle(STD_ERROR_HANDLE, console);
      }
   }
}

static void WriteVerbError(const char *text)
{
   HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
   DWORD written = 0;
   if (error && error != INVALID_HANDLE_VALUE)
      WriteFile(error, text, (DWORD) strlen(text), &written, NULL);
}

// The value after an option on the command line, "--config C:\x\y.ini",
// "--config=C:\x\y.ini", "--config "C:\a b\y.ini"" or, as PowerShell and cmd
// quote an argument with a space in it, ""--config=C:\a b\y.ini"" - a quoted
// value may hold spaces, which the verbs' own split at spaces cannot give. Empty
// when the option is not there or has no value.
static String CommandLineValueAfter(const String &commandLine, const String &option)
{
   String lower = commandLine;
   lower.MakeLower();
   String wanted = option;
   wanted.MakeLower();

   int at = lower.Find(wanted.c_str());
   while (at >= 0)
   {
      const int end = at + wanted.GetLength();
      const bool starts = at == 0 || commandLine[at - 1] == ' ' || commandLine[at - 1] == '"';
      const bool ends = end >= commandLine.GetLength() || commandLine[end] == ' ' || commandLine[end] == '=' || commandLine[end] == '"';
      if (starts && ends)
         break;
      at = lower.Find(wanted.c_str(), end);
   }

   if (at < 0)
      return String();

   const bool optionQuoted = at > 0 && commandLine[at - 1] == '"';

   int position = at + wanted.GetLength();
   if (position < commandLine.GetLength() && commandLine[position] == '=')
   {
      position++;

      // "--config=C:\a b\y.ini": the value runs to the quote that closes the
      // whole argument.
      if (optionQuoted)
      {
         const int close = commandLine.Find(_T("\""), position);
         if (close < 0)
            return commandLine.Mid(position);
         return commandLine.Mid(position, close - position);
      }
   }
   else
   {
      // "--config" "C:\a b\y.ini": past the quote that closes the option.
      if (optionQuoted && position < commandLine.GetLength() && commandLine[position] == '"')
         position++;

      while (position < commandLine.GetLength() && commandLine[position] == ' ')
         position++;
   }

   if (position >= commandLine.GetLength())
      return String();

   if (commandLine[position] == '"')
   {
      const int close = commandLine.Find(_T("\""), position + 1);
      if (close < 0)
         return commandLine.Mid(position + 1);
      return commandLine.Mid(position + 1, close - position - 1);
   }

   const int space = commandLine.Find(_T(" "), position);
   if (space < 0)
      return commandLine.Mid(position);
   return commandLine.Mid(position, space - position);
}

// For the verbs that rewrite, remove or add files in the data folder - the store
// walks but --measure-store - and every copy to or from an object store, one that
// only reads included, since it hands what it read to a store the service's
// account reads: whether a link the service could have made is anywhere in it. The
// data folder is the service's to write (Modify for NT SERVICE\hMailServer), these
// verbs run elevated - the walks with the service stopped, a copy without
// --remove-source beside it - and they reach each message by a
// path; a junction, a mount point or a symbolic link in that tree would carry an
// administrator's rewrite or removal to wherever it leads. So the folder is held
// from its drive's root down (HeldFolder, which held stays for as long as the
// walk runs, so it cannot be renamed under it), and every folder and file below
// it is listed once. A junction, a mount point or a symbolic link on the way or
// in it is gone through only when the administrators alone control it
// (OnlyAdministratorsControl: owned by Administrators, SYSTEM or
// TrustedInstaller, and nobody else may change it or remove it from its folder -
// a link an administrator made to keep the data, or a domain of it, on another
// drive), and where it leads is held and listed in the same way; any other stops
// the verb before it changes anything, named, with what makes it so. A folder no
// administrator may list - System Volume Information, when the data folder is a
// drive's root - is left unlisted when the administrators alone control it too:
// nobody else can have put a link in it. Reparse points that redirect nothing - a
// deduplicated or tiered file - are not links (IsReparseTagNameSurrogate). Where
// Windows has it, the process follows no junction a user who is not an
// administrator made anyway (ForbidServiceFolderWrites); this is the same rule on
// every Windows. docs/ServiceAccount.md, "What an administrator's command writes".
static HM::String ParentFolderOf(const HM::String &path)
{
   const int separator = path.ReverseFind(_T('\\'));
   if (separator <= 0)
      return HM::String();
   // C:\x's folder is C:\, not C:.
   return separator == 2 && path[1] == ':' ? path.Left(3) : path.Left(separator);
}

// What is at path - a link or a folder, opened itself and never through it - and
// the folder it is in, asked OnlyAdministratorsControl.
static bool AdministratorsAloneControl(const HM::String &path, HM::String &why)
{
   const HM::String parentPath = ParentFolderOf(path);
   // The error of the open that failed, asked at once: the other open sets it too.
   HANDLE object = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
   DWORD failed = object == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
   HANDLE parent = INVALID_HANDLE_VALUE;
   if (failed == ERROR_SUCCESS)
   {
      parent = parentPath.IsEmpty() ? INVALID_HANDLE_VALUE
         : CreateFileW(parentPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (parent == INVALID_HANDLE_VALUE)
         failed = parentPath.IsEmpty() ? ERROR_PATH_NOT_FOUND : GetLastError();
   }

   bool alone = false;
   if (object == INVALID_HANDLE_VALUE || parent == INVALID_HANDLE_VALUE)
      why = HM::Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) failed);
   else
      alone = HM::OnlyAdministratorsControl(object, parent, why);

   if (object != INVALID_HANDLE_VALUE)
      CloseHandle(object);
   if (parent != INVALID_HANDLE_VALUE)
      CloseHandle(parent);
   return alone;
}

static bool DataFolderHoldsNoLink(HM::HeldFolder &held, HM::String &why)
{
   HM::IniFileSettings::Instance()->LoadSettings();
   const HM::String dataFolder = HM::IniFileSettings::Instance()->GetDataDirectory();
   if (dataFolder.IsEmpty())
   {
      why = _T("hMailServer.INI names no data folder.");
      return false;
   }

   if (!held.Hold(dataFolder, false, why))
   {
      why = _T("the data folder could not be held: ") + why;
      return false;
   }

   std::vector<HM::String> pending;
   pending.push_back(held.Path());
   std::set<HM::String> followed;
   std::vector<HM::String> links;
   while (!pending.empty() && links.size() < 5)
   {
      const HM::String folder = pending.back();
      pending.pop_back();

      WIN32_FIND_DATAW found;
      const HM::String pattern = folder + _T("\\*");
      HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (search == INVALID_HANDLE_VALUE)
      {
         const DWORD error = GetLastError();
         // Gone since its parent was listed - a copy runs beside the service,
         // which may remove a folder it emptied - holds no link.
         if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            continue;

         // One no administrator may list, that only the administrators may
         // change - System Volume Information - holds nothing anybody else put
         // there.
         HM::String unlisted;
         if (error == ERROR_ACCESS_DENIED && folder != held.Path() && AdministratorsAloneControl(folder, unlisted))
            continue;

         why = HM::Formatter::Format(_T("{0} could not be listed (Windows error {1}), so whether it holds a link could not be told{2}."), folder, (int) error,
            unlisted.IsEmpty() ? HM::String() : HM::String(_T(", and ")) + unlisted);
         return false;
      }

      do
      {
         const HM::String name = found.cFileName;
         if (name == _T(".") || name == _T(".."))
            continue;

         const HM::String path = folder + _T("\\") + name;
         if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(found.dwReserved0))
         {
            // A link to a folder that only the administrators control is gone
            // through, where it leads held as the data folder is and listed in
            // its turn, once; a link to a file never is.
            HM::String untrusted;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
               untrusted = _T("a link to a file");
            else if (!AdministratorsAloneControl(path, untrusted))
               untrusted = untrusted.IsEmpty() ? HM::String(_T("its permissions could not be read")) : untrusted;
            else
            {
               HANDLE through = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
               HM::String heldWhy;
               if (through == INVALID_HANDLE_VALUE)
                  untrusted = HM::Formatter::Format(_T("where it leads could not be opened (Windows error {0})"), (int) GetLastError());
               else if (!held.HoldAlso(path, through, heldWhy))
                  untrusted = heldWhy;
               else
               {
                  std::vector<wchar_t> target(4096);
                  const DWORD length = GetFinalPathNameByHandleW(through, &target[0], (DWORD) target.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
                  HM::String key = length > 0 && length < target.size() ? HM::String(&target[0]) : path;
                  key.MakeLower();
                  if (followed.insert(key).second)
                     pending.push_back(path);
               }

               if (through != INVALID_HANDLE_VALUE)
                  CloseHandle(through);
            }

            if (!untrusted.IsEmpty())
               links.push_back(path + _T(" (") + untrusted + _T(")"));
         }
         else if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            pending.push_back(path);
      } while (FindNextFileW(search, &found));

      FindClose(search);
   }

   if (!links.empty())
   {
      HM::String named;
      for (const HM::String &link : links)
         named += (named.IsEmpty() ? _T("") : _T(", ")) + link;
      why = HM::Formatter::Format(_T("the data folder holds a junction, a mount point or a symbolic link that the administrators do not alone control: {0}. A rewrite run as an administrator would write through it. Find out what made it - the service's account may write there - and remove it; a link in the data folder is gone through only when it is owned by Administrators, SYSTEM or TrustedInstaller and nobody else may change it or remove it from its folder (docs/ServiceAccount.md)."), named);
      return false;
   }

   return true;
}

static int UpgradeDatabaseFromCommandLine()
{
   // The caller's console only when it gave no handles, as AttachVerbOutput: a
   // script's or the installer's redirected output is where what this says goes,
   // and since 6.3.6 it is the only place - an administrator's command writes
   // nothing to the error log (ForbidServiceFolderWrites). Until then a console
   // the caller had took the lines away from its redirection.
   if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL && AttachConsole(ATTACH_PARENT_PROCESS))
   {
      FILE *out = nullptr;
      freopen_s(&out, "CONOUT$", "w", stdout);
      FILE *err = nullptr;
      freopen_s(&err, "CONOUT$", "w", stderr);
   }

   // Refused while the service is running against the same database: a running
   // server plus this walker is two walkers on one schema (SchemaUpgrader locks
   // the version row against a second node, but a server that holds the old
   // schema in memory would go on running against a new one). Asked of the
   // service controller, not assumed; a service that is not installed, or that
   // cannot be queried, is not running.
   {
      bool serviceRunning = false;
      SC_HANDLE manager = OpenSCManager(NULL, NULL, GENERIC_READ);
      if (manager != NULL)
      {
         SC_HANDLE service = OpenService(manager, _T("hMailServer"), SERVICE_QUERY_STATUS);
         if (service != NULL)
         {
            SERVICE_STATUS status;
            memset(&status, 0, sizeof(status));
            if (QueryServiceStatus(service, &status))
               serviceRunning = status.dwCurrentState != SERVICE_STOPPED;
            CloseServiceHandle(service);
         }
         CloseServiceHandle(manager);
      }

      if (serviceRunning)
      {
         std::fprintf(stderr, "The hMailServer service is running. Stop it first (Stop-Service hMailServer), then run this again; a server running against a schema this walk changes under it would not know. Nothing was changed.\n");
         return 3;
      }
   }

   HM::IniFileSettings::Instance()->LoadSettings();
   if (!HM::IniFileSettings::Instance()->GetDatabaseSettingsExists())
   {
      std::fprintf(stderr, "[Database] in %s does not name a database. Nothing was changed.\n", HM::AnsiString(HM::IniFileSettings::GetInitializationFile()).c_str());
      return 2;
   }

   std::shared_ptr<HM::DatabaseSettings> settings = HM::SchemaUpgrader::SettingsFromIni();
   std::shared_ptr<HM::DALConnection> connection = HM::DALConnectionFactory::CreateConnection(settings);
   HM::String error;
   if (!connection || connection->Connect(error) != HM::DALConnection::Connected)
   {
      std::fprintf(stderr, "Could not connect to the database: %s\n", HM::AnsiString(error).c_str());
      return 1;
   }

   const HM::SchemaUpgrader::Outcome outcome = HM::SchemaUpgrader::Upgrade(connection, HM::IniFileSettings::Instance()->GetDBScriptDirectory(), REQUIRED_DB_VERSION,
      [](const HM::String &line) { std::printf("%s\n", HM::AnsiString(line).c_str()); });
   connection->Disconnect();

   if (!outcome.ok)
   {
      std::fprintf(stderr, "%s\nStopped at %s. The database is at %d; the steps before that one have been applied, and a second run resumes there.\n",
         HM::AnsiString(outcome.error).c_str(), HM::AnsiString(outcome.failed_step).c_str(), outcome.version);
      return 1;
   }

   if (outcome.steps == 0)
      std::printf("The database is at schema version %d, which is what this build needs. Nothing to do.\n", outcome.version);
   else
      std::printf("Upgraded in %d step%s; the database is at schema version %d.\n", outcome.steps, outcome.steps == 1 ? "" : "s", outcome.version);

   return 0;
}

static void WriteVerbOutput(const char *text)
{
   HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
   DWORD written = 0;
   if (output && output != INVALID_HANDLE_VALUE)
      WriteFile(output, text, (DWORD) strlen(text), &written, NULL);
}

// hMailServer.exe --set-admin-password: the administrator password written into
// [Security] AdministratorPassword of the hMailServer.ini beside this binary, as the
// salted PBKDF2 hash the Control Panel and COM write (IniFileSettings::
// SetAdministratorPassword), as `hmailserver --set-admin-password` writes it on
// Linux (platform/main.cpp). The Windows installer runs it: until 25 September 2026
// it wrote an unsalted MD5 of the password it collected into the file itself
// (docs/SecretsAtRest.md).
//
// The password is never an argument, where the process list and a script's own log
// would show it. It is the value of HM_ADMIN_PASSWORD in this process's
// environment - the installer's way, because Inno Setup starts a process but
// cannot write to its standard input - or, when that is not set, one line of
// standard input when that is a pipe or a file. No database is opened. A running
// service goes on with the password it loaded until it is restarted or
// reinitialised, which the last line says. Exit 0 written, 2 no password given, 1
// not written.
static int SetAdministratorPasswordFromCommandLine()
{
   AttachVerbOutput();

   HM::String password;

   // The environment first. It is what the installer sets, and when it is set the
   // standard input is not touched at all: a process started with an inherited
   // pipe that nobody writes to or closes would otherwise wait on it for ever -
   // and so would the installer waiting on this process.
   const DWORD needed = GetEnvironmentVariableW(L"HM_ADMIN_PASSWORD", NULL, 0);
   if (needed > 1)
   {
      std::vector<wchar_t> value((size_t) needed, L'\0');
      const DWORD length = GetEnvironmentVariableW(L"HM_ADMIN_PASSWORD", &value[0], needed);
      if (length > 0 && length < needed)
         password = HM::String(&value[0]);
      SecureZeroMemory(&value[0], value.size() * sizeof(wchar_t));
   }

   HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
   const DWORD inputType = (input == NULL || input == INVALID_HANDLE_VALUE) ? FILE_TYPE_UNKNOWN : GetFileType(input);

   if (password.IsEmpty() && (inputType == FILE_TYPE_PIPE || inputType == FILE_TYPE_DISK))
   {
      // One line, and no more than a password could be: what follows the first
      // line break is not read.
      std::string line;
      char buffer[512];
      DWORD read = 0;
      while (line.size() < 4096 && ReadFile(input, buffer, sizeof(buffer), &read, NULL) && read > 0)
      {
         line.append(buffer, read);
         if (line.find_first_of("\r\n") != std::string::npos)
            break;
      }
      SecureZeroMemory(buffer, sizeof(buffer));

      std::string::size_type end = line.find_first_of("\r\n");
      if (end == std::string::npos)
         end = line.size();

      // A byte order mark a writer put in front of the text is not part of it.
      std::string::size_type start = 0;
      if (end >= 3 && (unsigned char) line[0] == 0xEF && (unsigned char) line[1] == 0xBB && (unsigned char) line[2] == 0xBF)
         start = 3;

      if (end > start)
      {
         const int length = (int) (end - start);
         const int wideNeeded = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data() + start, length, NULL, 0);
         if (wideNeeded <= 0)
         {
            SecureZeroMemory(&line[0], line.size());
            WriteVerbError("The password on standard input is not UTF-8 text, so nothing was changed.\r\n");
            return 2;
         }

         std::vector<wchar_t> wide((size_t) wideNeeded + 1, L'\0');
         MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data() + start, length, &wide[0], wideNeeded);
         password = HM::String(&wide[0]);
         SecureZeroMemory(&wide[0], wide.size() * sizeof(wchar_t));
      }

      // Overwrite this process's narrow copy, as the Linux verb does.
      if (!line.empty())
         SecureZeroMemory(&line[0], line.size());
   }

   if (password.IsEmpty())
   {
      WriteVerbError("No password was given, so nothing was changed. Give it in the environment variable HM_ADMIN_PASSWORD, or as one line of standard input - never as an argument, where the process list would show it.\r\n");
      return 2;
   }

   // The settings first, so that the hash is derived with the work factor this
   // server is configured for (PasswordHashIterations), as the Linux verb does.
   HM::IniFileSettings::Instance()->LoadSettings();
   HM::IniFileSettings::Instance()->SetAdministratorPassword(password);

   // Read back from the file itself, as the service will read it when it starts -
   // the file flushed, and the settings loaded from it again - and held to being a
   // PBKDF2 hash of this password.
   const HM::String file = HM::IniFileSettings::GetInitializationFile();
   WritePrivateProfileString(NULL, NULL, NULL, file.c_str());

   HM::IniFileSettings::Instance()->LoadSettings();
   const HM::String storedHash = HM::IniFileSettings::Instance()->GetAdministratorPassword();

   const bool written = HM::Crypt::Instance()->GetHashType(storedHash) == HM::Crypt::ETPBKDF2 &&
                        HM::Crypt::Instance()->Validate(password, storedHash, HM::Crypt::ETPBKDF2);

   if (!written)
   {
      std::string message = "The administrator password could not be written to " + std::string(HM::AnsiString(file).c_str()) +
                            ". The file must be writable by the account this runs as; run it from an elevated prompt.\r\n";
      WriteVerbError(message.c_str());
      return 1;
   }

   std::string message = "The administrator password was written to " + std::string(HM::AnsiString(file).c_str()) +
                         " as a PBKDF2 hash. A running hMailServer service takes it when it is next started or reinitialised.\r\n";
   WriteVerbOutput(message.c_str());
   return 0;
}


// The stored-secrets key's verbs (Common/Util/StoredSecrets.h,
// docs/SecretsAtRest.md), the export and the import each with the key file as the
// argument after it, the forget with the lost key's id. Read with
// CommandLineToArgvW rather than from the space-split line the other verbs use,
// so that a quoted path with a space in it is one argument.
static bool FindVerbWithArgument(const wchar_t *verb, HM::String &file)
{
   file.Empty();

   int count = 0;
   LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
   if (arguments == NULL)
      return false;

   bool present = false;
   for (int index = 1; index < count; index++)
   {
      if (_wcsicmp(arguments[index], verb) == 0)
      {
         present = true;
         if (index + 1 < count)
            file = arguments[index + 1];
         break;
      }
   }

   LocalFree(arguments);
   return present;
}

static std::string NarrowForVerb(const HM::String &text)
{
   return std::string(HM::AnsiString(text).c_str());
}

// The database the key is recorded in, opened as the other verbs open it. False,
// said, when it cannot be.
static bool OpenDatabaseForKeyVerb()
{
   HM::String error;
   if (!HM::Application::Instance()->OpenDatabase(error))
   {
      WriteVerbError(("The database could not be opened: " + NarrowForVerb(error) + ". Nothing was changed.\r\n").c_str());
      return false;
   }
   return true;
}

// hMailServer.exe --create-stored-secrets-key: this installation's stored-secrets
// key made, written into [Security] StoredSecretsKey of the hMailServer.ini beside
// this binary under this machine's DPAPI, and recorded in the database - the only
// ways a key is made, besides the Linux --create-database; the service never makes
// one. The installer runs it on a new installation, after the database is created;
// an administrator runs it once, elevated, to turn sealing on for an installation
// upgraded from before 25 September 2026. Refused where the database records a key
// or holds values sealed under one: that key is imported, never replaced. Exit 0
// made (or already held, and the database's), 1 not made.
static int CreateStoredSecretsKeyFromCommandLine()
{
   AttachVerbOutput();

   HM::IniFileSettings::Instance()->LoadSettings();

   if (!OpenDatabaseForKeyVerb())
      return 1;

   HM::AnsiString keyId;
   bool alreadyHeld = false;
   HM::String problem;
   const bool made = HM::StoredSecrets::CreateKey(keyId, alreadyHeld, problem);
   HM::Application::Instance()->CloseDatabase();

   if (!made)
   {
      WriteVerbError(("No stored-secrets key was made: " + NarrowForVerb(problem) + ". Nothing was changed.\r\n").c_str());
      return 1;
   }

   if (alreadyHeld)
   {
      WriteVerbOutput(("This server already holds the stored-secrets key " + std::string(keyId.c_str()) + ", which is the database's. Nothing was changed.\r\n").c_str());
      return 0;
   }

   WriteVerbOutput(("Made the stored-secrets key " + std::string(keyId.c_str()) + ", wrote it to [Security] StoredSecretsKey in " +
      NarrowForVerb(HM::IniFileSettings::GetInitializationFile()) + " under this machine's DPAPI, and recorded it in the database. "
      "The service seals an account's second factor and the [Settings] secrets under it from its next start or reinitialise.\r\n"
      "Export it NOW - hMailServer.exe --export-stored-secrets-key FILE, from an elevated prompt - and keep the file with the database backups, stored apart from them as the password pepper is: "
      "the key exists on this machine alone, and a database restored anywhere without it opens no second factor and no [Settings] secret. "
      "Every other node that shares this database needs it too (--import-stored-secrets-key FILE there).\r\n").c_str());
   return 0;
}

// hMailServer.exe --export-stored-secrets-key FILE: this server's stored-secrets
// key written to FILE - a new file, readable by SYSTEM and the administrators only
// (ProtectedFile::WriteNew, which never replaces a file), never printed - to keep
// with the database backups, and for --import-stored-secrets-key on every other
// node that shares the database. Checked against the key the database records, and
// the export recorded there for the setup checklist; a database that cannot be
// reached does not stop the copy - that is when one is wanted most - but then the
// export is not recorded. It makes no key. Exit 0 written, 2 refused before
// anything was done, 1 failed.
static int ExportStoredSecretsKeyFromCommandLine(const HM::String &file)
{
   AttachVerbOutput();

   if (file.IsEmpty() || file.Left(2) == _T("--"))
   {
      WriteVerbError("--export-stored-secrets-key needs the file to write the key to; for example --export-stored-secrets-key C:\\Keys\\hmailserver-stored-secrets.key.\r\n");
      return 2;
   }

   if (HM::FileUtilities::Exists(file))
   {
      WriteVerbError((NarrowForVerb(file) + " already exists. Name a file that does not, so that nothing is written over. Nothing was changed.\r\n").c_str());
      return 2;
   }

   HM::IniFileSettings::Instance()->LoadSettings();

   HM::String error;
   const bool databaseOpen = HM::Application::Instance()->OpenDatabase(error);

   // The key file's text made and written by StoredSecrets::ExportFile, which
   // writes it over however it ends.
   HM::AnsiString keyId;
   HM::String problem;
   HM::String writeError;
   const bool written = HM::StoredSecrets::ExportFile(file, keyId, problem, writeError);
   if (!written && writeError.IsEmpty())
   {
      if (databaseOpen)
         HM::Application::Instance()->CloseDatabase();
      WriteVerbError((NarrowForVerb(problem) + ". Nothing was written.\r\n").c_str());
      return 1;
   }

   const bool recorded = written && databaseOpen && HM::StoredSecrets::RecordExport(keyId);
   if (databaseOpen)
      HM::Application::Instance()->CloseDatabase();

   if (!written)
   {
      WriteVerbError(("The key could not be written to " + NarrowForVerb(file) + ": " + NarrowForVerb(writeError) + "\r\n").c_str());
      return 1;
   }

   WriteVerbOutput(("The stored-secrets key " + std::string(keyId.c_str()) + " was written to " + NarrowForVerb(file) +
      ", readable by SYSTEM and the administrators only. Keep it with the database backups, stored apart from them as the password pepper is: "
      "a database restored without it opens no second factor and no [Settings] secret, and it opens every one of them for whoever holds it. "
      "On every other node that shares this server's database: hMailServer.exe --import-stored-secrets-key FILE from an elevated prompt, then restart the service there.\r\n").c_str());

   if (!recorded)
   {
      WriteVerbError(("The export could not be recorded in the database" + std::string(databaseOpen ? "" : (": " + NarrowForVerb(error)).c_str()) +
         ", so the setup checklist still asks for one; export it again, to a new file, once the database can be reached.\r\n").c_str());
   }

   return 0;
}

// hMailServer.exe --import-stored-secrets-key FILE: the key in FILE made this
// server's - written into [Security] StoredSecretsKey under this machine's DPAPI -
// on a standby, a new machine, or any node that shares the database with the one
// that made it. Checked against the key the database records, and refused where
// another key that opens is held: replacing it is removing its line first,
// deliberately. The service takes it when it next starts or is reinitialised. Exit
// 0 imported, 2 refused before anything was done, 1 failed.
static int ImportStoredSecretsKeyFromCommandLine(const HM::String &file)
{
   AttachVerbOutput();

   if (file.IsEmpty() || file.Left(2) == _T("--"))
   {
      WriteVerbError("--import-stored-secrets-key needs the key file --export-stored-secrets-key wrote.\r\n");
      return 2;
   }

   if (!HM::FileUtilities::Exists(file))
   {
      WriteVerbError((NarrowForVerb(file) + " does not exist. Nothing was changed.\r\n").c_str());
      return 2;
   }

   HM::IniFileSettings::Instance()->LoadSettings();

   if (!OpenDatabaseForKeyVerb())
      return 1;

   // Read and imported by StoredSecrets::ImportFile, which reads the file into
   // one buffer and writes it, and every copy made of it, over however it ends.
   HM::AnsiString keyId;
   HM::String problem;
   const bool imported = HM::StoredSecrets::ImportFile(file, keyId, problem);
   HM::Application::Instance()->CloseDatabase();

   if (!imported)
   {
      WriteVerbError(("The key was not imported: " + NarrowForVerb(problem) + ". Nothing was changed.\r\n").c_str());
      return 1;
   }

   WriteVerbOutput(("The stored-secrets key " + std::string(keyId.c_str()) + " is this server's, in [Security] StoredSecretsKey of " +
      NarrowForVerb(HM::IniFileSettings::GetInitializationFile()) + ". Restart the hMailServer service, or reinitialise it, for it to take the key. Keep " +
      NarrowForVerb(file) + " with the database backups, or delete it if a copy is kept there already.\r\n").c_str());
   return 0;
}

// hMailServer.exe --forget-sealed-secrets KEYID: the way forward when a
// stored-secrets key is lost - no node holds it any more and no export was kept.
// Importing it from a node that has it is always the first thing to try. Every
// envelope that names that key is cleared: an account's second factor removed, so
// that it enrols again; a [Settings] secret emptied, so that it is entered again.
// The database's record is removed where it names that key, after which
// --create-stored-secrets-key makes a new one. Nothing sealed under another key is
// touched. Refused where this node's own key line does not open, and where this
// node holds the key named. Each item is printed, and all of them said again in
// one entry, HM7497, on standard error - this process writes no log
// (ForbidServiceFolderWrites). Refused while the service runs, as the profile verb is: a
// running server holds the accounts and settings it read, and would write an
// envelope back. Exit 0 done, 1 refused or failed, 2 no key id, 4 the service is
// running.
static int ForgetSealedSecretsFromCommandLine(const HM::String &keyId)
{
   AttachVerbOutput();

   if (keyId.IsEmpty() || keyId.Left(2) == _T("--"))
   {
      WriteVerbError("--forget-sealed-secrets needs the lost key's id, the sixteen hex digits HM7495 and the setup checklist name: --forget-sealed-secrets 0123456789abcdef. Import the key from a node that holds it instead, if there is one. Nothing was changed.\r\n");
      return 2;
   }

   if (HMailServerServiceIsRunning())
   {
      WriteVerbError("The hMailServer service is running. Stop it (Stop-Service hMailServer), run this again, then start it. Nothing was changed.\r\n");
      return 4;
   }

   HM::IniFileSettings::Instance()->LoadSettings();

   if (!OpenDatabaseForKeyVerb())
      return 1;

   std::vector<HM::String> forgotten;
   HM::String problem;
   const bool done = HM::StoredSecrets::ForgetLostKey(HM::AnsiString(keyId), forgotten, problem);
   HM::Application::Instance()->CloseDatabase();

   for (const HM::String &line : forgotten)
      WriteVerbOutput((NarrowForVerb(line) + "\r\n").c_str());

   if (!done)
   {
      WriteVerbError(((forgotten.empty() ? std::string("Nothing was cleared: ") : std::string("Not everything was cleared: ")) + NarrowForVerb(problem) + ".\r\n").c_str());
      return 1;
   }

   if (forgotten.empty())
      WriteVerbOutput(("Nothing is sealed under the key " + NarrowForVerb(keyId) + ", and the database does not record it. Nothing was changed.\r\n").c_str());
   else
      WriteVerbOutput("Done. Start the service. If this node holds no stored-secrets key now, make one with hMailServer.exe --create-stored-secrets-key and export it at once.\r\n");

   return 0;
}


// A sentence of the server's, to the verb's error handle in UTF-8.
static void WriteVerbError(const HM::String &text)
{
   HM::AnsiString narrow;
   HM::Unicode::WideToMultiByte(text + _T("\r\n"), narrow);
   WriteVerbError(narrow.c_str());
}

// The switches of /GrantServiceAccess (HM::ServiceAccount::GrantOptions),
// read with the command line's own quoting - the split on spaces below would
// cut a folder with a space in it. False, with why, for a switch without the
// folder it takes.
static bool ReadGrantOptions(HM::ServiceAccount::GrantOptions &options, HM::String &why)
{
   int count = 0;
   LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
   if (arguments == nullptr)
   {
      why = _T("The command line could not be read.");
      return false;
   }

   bool read = true;
   for (int index = 1; index < count && read; index++)
   {
      const HM::String argument = arguments[index];
      if (argument.CompareNoCase(_T("--confirm")) == 0)
         options.confirm = true;
      else if (argument.CompareNoCase(_T("--report-only")) == 0)
         options.reportOnly = true;
      else if (argument.CompareNoCase(_T("--approve-modify")) == 0 || argument.CompareNoCase(_T("--approve-read")) == 0)
      {
         const bool modify = argument.CompareNoCase(_T("--approve-modify")) == 0;
         const HM::String folder = index + 1 < count ? HM::String(arguments[index + 1]) : HM::String();
         if (folder.IsEmpty() || folder.Left(1) == _T("/") || folder.Left(2) == _T("--"))
         {
            why = HM::Formatter::Format(_T("{0} needs the folder it approves, in quotes when it has a space in it; for example {0} \"D:\\Mail Backups\". Nothing was changed."), argument);
            read = false;
            break;
         }
         options.approve.push_back(std::make_pair(folder, modify ? (DWORD) HM::ServiceAccount::RightsModify : (DWORD) HM::ServiceAccount::RightsReadExecute));
         index++;
      }
   }

   LocalFree(arguments);
   return read;
}

// hMailServer.exe /SetAdministratorPassword: the administrator password,
// read from standard input and written into hMailServer.INI as the PBKDF2
// hash the Control Panel's card writes - the Windows side of
// hmailserver --set-admin-password. It exists because the Windows service's
// own account may not write the file (docs/ServiceAccount.md), so the card
// cannot change the password through the service; an administrator changes
// it here, and restarts the service. Standard input, not an argument, so that
// the password is in no process list or shell history:
//
//    $p = Read-Host -AsSecureString 'New administrator password'
//    [Net.NetworkCredential]::new('', $p).Password | & '.\hMailServer.exe' /SetAdministratorPassword
//
// Exit 0 written, 1 not written, 2 no password given, 3 the file cannot be
// written by this account.
static int SetAdministratorPasswordFromInput()
{
   HM::IniFileSettings::Instance()->LoadSettings();

   if (!HM::IniFileSettings::InitializationFileWritable())
   {
      WriteVerbError(HM::Formatter::Format(_T("{0} cannot be written by this account: run this from an elevated prompt. Nothing was changed."), HM::IniFileSettings::GetInitializationFile()));
      return 3;
   }

   HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
   std::string line;
   if (input != NULL && input != INVALID_HANDLE_VALUE)
   {
      char character = 0;
      DWORD read = 0;
      while (line.size() < 4096 && ReadFile(input, &character, 1, &read, NULL) && read == 1 && character != '\n')
         line += character;
   }

   while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();

   if (line.empty())
   {
      WriteVerbError("No password was given on standard input; nothing was changed. From PowerShell: $p = Read-Host -AsSecureString 'New administrator password'; [Net.NetworkCredential]::new('', $p).Password | & '.\\hMailServer.exe' /SetAdministratorPassword\r\n");
      return 2;
   }

   HM::String password;
   HM::Unicode::MultiByteToWide(HM::AnsiString(line.c_str()), password);
   std::fill(line.begin(), line.end(), '\0');

   const bool written = HM::IniFileSettings::Instance()->SetAdministratorPassword(password);
   password.assign(password.GetLength(), _T('\0'));

   if (!written)
   {
      WriteVerbError(HM::Formatter::Format(_T("The administrator password could not be written to {0}; nothing was changed."), HM::IniFileSettings::GetInitializationFile()));
      return 1;
   }

   HM::AnsiString said;
   HM::Unicode::WideToMultiByte(HM::Formatter::Format(_T("The administrator password was written to {0} as a PBKDF2 hash. Restart the hMailServer service for it to take effect.\r\n"), HM::IniFileSettings::GetInitializationFile()), said);
   HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
   DWORD written_ = 0;
   if (output && output != INVALID_HANDLE_VALUE)
      WriteFile(output, said.c_str(), (DWORD) said.GetLength(), &written_, NULL);
   return 0;
}

// --pst-read <handle>: this program as the PST reader's process, which only the
// service starts (Common/Pst/PstReaderChild.h, PstReaderProcess.h). Asked
// before anything else is done, so that the reader arms no crash oracle, reads
// no configuration, registers nothing and opens no database: it has the file's
// handle and two pipes, and nothing else of the service's. False, with nothing
// done, for any other command line.
static bool RunAsPstReader(int &exitCode)
{
   int count = 0;
   LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
   if (arguments == NULL)
      return false;

   const bool reader = count == 3 && lstrcmpiW(arguments[1], L"--pst-read") == 0;
   std::string handle;
   if (reader)
   {
      for (const wchar_t *digit = arguments[2]; *digit != L'\0'; digit++)
         handle += (*digit >= L'0' && *digit <= L'9') ? (char) *digit : '?';
   }
   LocalFree(arguments);

   if (!reader)
      return false;

   exitCode = HM::PstReaderChild::Run(handle);
   return true;
}

extern "C" int WINAPI _tWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                                LPTSTR lpCmdLine, int nShowCmd)
{
   int readerExitCode = 0;
   if (RunAsPstReader(readerExitCode))
      return readerExitCode;

   // The two verbs of an administrator's install of an update
   // (Common/Util/AdminUpdate.h), which run elevated from the Control Panel's
   // Install update: /InstallUpdate, the stage that checks the installer, and
   // --verify-bundle, its Sigstore check run with the administrator's rights
   // taken away. Here, before the crash oracle, COM, RegisterAppID and anything
   // that reads the configuration, as the PST reader is: neither may write where
   // the service can - a crash dump, a log line or an error in the log folder -
   // and neither needs anything of the server's but the code that checks. Each
   // limits DLL loading to Bin and System32 and forbids the logger's writes as
   // its first act.
   if (HM::AdminUpdate::IsVerifyBundleCommandLine())
      return HM::AdminUpdate::RunVerifyBundleChild();
   if (HM::AdminUpdate::IsInstallUpdateCommandLine())
      return HM::AdminUpdate::RunInstallUpdateCommand();

   // Every other verb an administrator runs - /Register, /Unregister,
   // /RegisterTypeLib, --apply-new-install-defaults, --apply-profile,
   // --create-stored-secrets-key, --upgrade-database and the rest below -
   // writes nothing where the service can, from here on, and has no crash
   // oracle (ForbidServiceFolderWrites). Until 6.3.6 only the update's two
   // verbs, the grant and the database login did so; the others wrote their
   // errors, and a failed /Register its own, to the error log in the log folder,
   // and every one of them armed the crash oracle there.
   if (!IsServerCommandLine())
      ForbidServiceFolderWrites();
   else
   {
      // Arm the crash oracle before anything else runs. Both of its hooks are
      // process-wide, so installing them here covers every thread the server creates
      // later, including the ones the service control manager and boost::asio start on
      // our behalf. It touches nothing but the ini file, so it is safe this early -
      // and it has to be this early, because a fault during startup is exactly the kind
      // that otherwise disappears.
      HM::CrashOracle::Install();
   }

   // Initailize some service variables.
   ServiceStatus.dwServiceType = SERVICE_WIN32; 
   ServiceStatus.dwWin32ExitCode = 0;
   ServiceStatus.dwServiceSpecificExitCode = 0; 
   ServiceStatus.dwCheckPoint = 0; 
   ServiceStatus.dwWaitHint = 0; 

   // Parse the command line.
   // The mode is the last token of the command line, and the line is
   // trimmed first: a launcher that appends a space to the arguments -
   // OpenCppCoverage hands its child "/Debug " - used to leave an empty
   // last token, which was no mode at all, and the process ended at once
   // with nothing said (15 September 2026, the first coverage run of 6.3.3).
   HM::String sCommandLine = GetCommandLine();
   sCommandLine.Trim();
   std::vector<String> vecParams = StringParser::SplitString(sCommandLine, " ");
   String sLastParam;
   for (auto param = vecParams.rbegin(); param != vecParams.rend(); ++param)
   {
      if (!param->IsEmpty())
      {
         sLastParam = *param;
         break;
      }
   }

   // --apply-profile NAME [--confirm] [--once] [--if-new]: the one verb that takes a
   // value, so it is found anywhere on the line rather than as its last token. The
   // name is the token after it; the switches may stand anywhere. --revert-profile
   // takes none.
   bool profileVerb = false;
   bool profileRevert = false;
   bool profileConfirmed = false;
   bool profileOnce = false;
   bool profileIfNew = false;
   String profileName;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--apply-profile")) == 0)
      {
         profileVerb = true;
         for (size_t next = index + 1; next < vecParams.size() && profileName.IsEmpty(); next++)
         {
            if (!vecParams[next].IsEmpty())
               profileName = vecParams[next];
         }
      }
      else if (vecParams[index].CompareNoCase(_T("--revert-profile")) == 0)
         profileRevert = true;
      else if (vecParams[index].CompareNoCase(_T("--confirm")) == 0)
         profileConfirmed = true;
      else if (vecParams[index].CompareNoCase(_T("--once")) == 0)
         profileOnce = true;
      else if (vecParams[index].CompareNoCase(_T("--if-new")) == 0)
         profileIfNew = true;
   }

   // --apply-new-install-defaults: what a new installation starts with
   // (Common/Application/NewInstallDefaults.h, docs/SecureDefaults.md). A switch
   // with no value, found anywhere on the line as the profile verb is.
   bool newInstallDefaultsVerb = false;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--apply-new-install-defaults")) == 0)
         newInstallDefaultsVerb = true;
   }

   // The message store walked (Common/Persistence/MessageStoreMaintenance.h):
   // --measure-store, --compress-store [--decompress] and --recount-store, the
   // first two with an optional --account <address> or --domain <name>, found
   // anywhere on the line as the profile verb's switches are; and the two walks
   // of mailbox encryption at rest, --encrypt-store and --decrypt-store, which
   // take the same two.
   int storeVerb = 0;
   String storeAccount;
   bool storeAccountGiven = false;
   String storeDomain;
   bool storeDomainGiven = false;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--measure-store")) == 0)
         storeVerb = HM::MessageStoreMaintenance::VerbMeasure;
      else if (vecParams[index].CompareNoCase(_T("--compress-store")) == 0 && storeVerb != HM::MessageStoreMaintenance::VerbDecompress)
         storeVerb = HM::MessageStoreMaintenance::VerbCompress;
      else if (vecParams[index].CompareNoCase(_T("--recount-store")) == 0)
         storeVerb = HM::MessageStoreMaintenance::VerbRecount;
      else if (vecParams[index].CompareNoCase(_T("--encrypt-store")) == 0)
         storeVerb = HM::MessageStoreMaintenance::VerbEncrypt;
      else if (vecParams[index].CompareNoCase(_T("--decrypt-store")) == 0)
         storeVerb = HM::MessageStoreMaintenance::VerbDecrypt;
      else if (vecParams[index].CompareNoCase(_T("--account")) == 0)
      {
         storeAccountGiven = true;
         for (size_t next = index + 1; next < vecParams.size() && storeAccount.IsEmpty(); next++)
         {
            if (!vecParams[next].IsEmpty())
               storeAccount = vecParams[next];
         }
      }
      else if (vecParams[index].CompareNoCase(_T("--domain")) == 0)
      {
         storeDomainGiven = true;
         for (size_t next = index + 1; next < vecParams.size() && storeDomain.IsEmpty(); next++)
         {
            if (!vecParams[next].IsEmpty())
               storeDomain = vecParams[next];
         }
      }
   }
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--decompress")) == 0 && storeVerb == HM::MessageStoreMaintenance::VerbCompress)
         storeVerb = HM::MessageStoreMaintenance::VerbDecompress;
   }

   // --service-account virtual|localsystem|keep|DOMAIN\name$, beside /Register:
   // the account the installer's page or the Control Panel chose
   // (docs/ServiceAccount.md), found anywhere on the line as the other switches
   // are. One word each, so that nothing with a space in it - NT
   // SERVICE\hMailServer - has to survive the split on spaces above. Without it
   // /Register keeps the account a service has, and makes a new one
   // LocalSystem. It is the ONLY place the account comes from: never
   // ServiceAccountName in hMailServer.INI, which others write (ServiceManager.h).
   bool serviceAccountGiven = false;
   String serviceAccountWord;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--service-account")) == 0)
      {
         serviceAccountGiven = true;
         for (size_t next = index + 1; next < vecParams.size() && serviceAccountWord.IsEmpty(); next++)
         {
            if (!vecParams[next].IsEmpty())
               serviceAccountWord = vecParams[next];
         }
      }
   }

   // --check-database [--config <file>]: one connection to the database the
   // configuration names, with its TLS settings, and a line saying how it is
   // protected or why there is none (Common/SQL/DatabaseTls, docs/DatabaseTls.md).
   // Found anywhere on the line, as the store verbs are. --config names another
   // configuration file than the installed one, for this verb only - which is how
   // a TlsMode is tried before the service is given it.
   bool checkDatabaseVerb = false;
   bool configGiven = false;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      // The split at spaces keeps quotes: "--config=C:\a b\x.ini" arrives as
      // "--config=C:\a and b\x.ini".
      String parameter = vecParams[index];
      if (parameter.Left(1) == _T("\""))
         parameter = parameter.Mid(1);
      if (parameter.Right(1) == _T("\""))
         parameter = parameter.Left(parameter.GetLength() - 1);

      if (parameter.CompareNoCase(_T("--check-database")) == 0)
         checkDatabaseVerb = true;
      else if (parameter.CompareNoCase(_T("--config")) == 0 || parameter.Left(9).CompareNoCase(_T("--config=")) == 0)
         configGiven = true;
   }
   const String checkDatabaseConfig = CommandLineValueAfter(sCommandLine, _T("--config"));

   // --config names another file for --check-database and for nothing else: every
   // other command, and the service, read the installed file whatever the line
   // says, so a --config beside one of them is refused rather than ignored - it
   // would otherwise upgrade, measure or change the installed database while the
   // administrator believed they had pointed it elsewhere.
   if (configGiven && !checkDatabaseVerb)
   {
      AttachVerbOutput();
      WriteVerbError("--config goes with --check-database only; every other command reads the installed hMailServer.ini. Nothing was done.\r\n");
      return 2;
   }

   // The message store copied to an object store or back from one
   // (Common/Persistence/MessageStoreObjects.h, docs/ObjectStorage.md), with the
   // walk's --account and --domain, and --remove-source.
   bool copyToObjects = false;
   bool copyFromObjects = false;
   bool removeSource = false;
   for (size_t index = 0; index < vecParams.size(); index++)
   {
      if (vecParams[index].CompareNoCase(_T("--copy-store-to-object-storage")) == 0)
         copyToObjects = true;
      else if (vecParams[index].CompareNoCase(_T("--copy-store-from-object-storage")) == 0)
         copyFromObjects = true;
      else if (vecParams[index].CompareNoCase(_T("--remove-source")) == 0)
         removeSource = true;
   }

   _AtlModule.InitializeCom();

   // /Unregister is decided before anything below registers anything, because it
   // removes what they write. Until 25 September 2026 it came after the
   // RegisterAppID call below, so every /Unregister first wrote
   // HKCR\AppID\{5EDEC473-39E0-43F6-A234-1947071721C8} with LocalService=hMailServer,
   // and one that found nothing else registered - UnregisterServer then fails, and
   // UnregisterAppId was skipped when it did - left that key behind. An uninstall
   // after an upgrade runs /Unregister twice, once for each installation's
   // [UninstallRun] entries, and that is how the installer smoke test found it.
   // Now it creates nothing: the service, the COM server and the AppID are removed,
   // the AppID whether or not the type library was still registered, and then the
   // Application log's event source (WindowsEventLog::UnregisterSource), last, so
   // that nothing on this path can write an event after it and put it back.
   if (sLastParam.CompareNoCase(_T("/Unregister")) == 0)
   {
      AttachVerbOutput();

      // ---  Unregister the hMailServer service.
      HM::ServiceManager hSCM;
      hSCM.UnregisterService("hMailServer");

      _AtlModule.UnregisterServer(TRUE);
      _AtlModule.UnregisterAppId();

      // The auto-ban's Windows Defender Firewall rules (AutoBanFirewall=1), which
      // Windows keeps across restarts: with the server gone nothing would ever
      // remove them, and each would block its address on these ports for good,
      // whatever listens on them next. The uninstaller runs this elevated; a
      // rule it cannot remove is said on standard error, the only place left.
      HM::String firewallError;
      if (HM::AutoBanFirewall::RemoveAllWindowsRules(firewallError) < 0)
         fwprintf(stderr, L"hMailServer: the auto-ban's firewall rules (group \"%s\") could not all be removed: %s\n", HM::AutoBanFirewall::RuleGroup(), firewallError.c_str());

      HM::WindowsEventLog::UnregisterSource();

      return 0;
   }

   // Register app id so that client can create instances
   // of us even when running as a service under the local
   // system account.
   _AtlModule.RegisterAppID();

   // Run registration of the server and service (/Unregister is above).
   bool bRegisterService = sLastParam.CompareNoCase(_T("/Register")) == 0;
   bool bRegisterTypelib = bRegisterService || (sLastParam.CompareNoCase(_T("/RegisterTypeLib")) == 0);
   if (bRegisterService)
   {
      // What a failed registration says goes on this verb's own output - the
      // installer, DBSetup and the Control Panel keep it - and never to the
      // error log, which this process does not write (ForbidServiceFolderWrites).
      // Until 6.3.6 the settings were loaded here to find the log folder for it.
      AttachVerbOutput();

      // --- Register the hMailServer service.
      HM::ServiceManager hSCM;

      if (serviceAccountGiven)
      {
         HM::ServiceManager::AccountChoice choice = HM::ServiceManager::AccountManaged;
         if (serviceAccountWord.CompareNoCase(_T("virtual")) == 0)
            choice = HM::ServiceManager::AccountVirtual;
         else if (serviceAccountWord.CompareNoCase(_T("localsystem")) == 0)
            choice = HM::ServiceManager::AccountLocalSystem;
         else if (serviceAccountWord.CompareNoCase(_T("keep")) == 0)
            choice = HM::ServiceManager::AccountKeep;

         // Checked before anything is registered, so that a word that is none
         // of them is said and changes nothing.
         HM::ServiceManager::Logon logon;
         HM::String refusal;
         if (!HM::ServiceManager::LogonFor(choice, _T("hMailServer"), serviceAccountWord, logon, refusal))
         {
            AttachVerbOutput();
            WriteVerbError(refusal);
            return -1;
         }

         hSCM.SetAccountChoice(choice, serviceAccountWord);
      }

      const bool registered = hSCM.RegisterService("hMailServer", "hMailServer");
      if (!hSCM.GetReported().IsEmpty())
         WriteVerbError(hSCM.GetReported());
      if (!registered)
      {
         WriteVerbError("The hMailServer service was not created or changed.\r\n");
         return -1;
      }

      // The event source, while this is elevated: a service that is not
      // LocalSystem cannot write HKLM, and finds the source already there
      // (WindowsEventLog::RegisterSource).
      HM::WindowsEventLog::RegisterSource();
   }

   if (bRegisterTypelib)
   {
      AttachVerbOutput();

      HRESULT registration = _AtlModule.RegisterAppId();
      if (SUCCEEDED(registration))
         registration = _AtlModule.RegisterServer(TRUE);
      if (FAILED(registration))
      {
         char said[200];
         sprintf_s(said, "The COM server and its type library could not be registered (HRESULT 0x%08lX); run this from an elevated prompt.\r\n", (unsigned long) registration);
         WriteVerbError(said);
         return -1;
      }

      return 0;
   }


   // In Windows 2000/XP, we need to register objects directly
   // after service start (that is now). Otherwise it won't 
   // work. In later versions, we must register them after the 
   // service has notified the SCM about the status. 
   if (SystemInformation::GetOperatingSystem() == SystemInformation::Windows2000 ||
      SystemInformation::GetOperatingSystem() == SystemInformation::WindowsXP ||
      SystemInformation::GetOperatingSystem() == SystemInformation::WindowsNT)
   {
      _AtlModule.RegisterObjects();
   }
   
   int iRet = 0;
   String secretsKeyFile;

   if (sLastParam.CompareNoCase(_T("/Test")) == 0)
   {
      // --- We are running in debug mode.
      //     This is a bit ugly but should work fine
      //     for debugging purposes.
      DEBUG_MODE = true;
   
      String sErrorMessage;
      if (HM::Application::Instance()->InitInstance(sErrorMessage))
      {
         // We were able to connect to the database server
         // and the database is of the correct version. So
         // now we should start the TCP/IP-servers.
         HM::Application::Instance()->StartServers();
      }

      ClassTester oTester;
      oTester.DoTests();

      // A thread the stop gave up on, or a connection left open for its
      // caller, ends with the process, now, as after a service stop
      // (TerminateServer).
      if (!HM::Application::Instance()->ShutDown())
      {
         fflush(nullptr);
         TerminateProcess(GetCurrentProcess(), 0);
      }
   }
   else if (sLastParam.CompareNoCase(_T("/Debug")) == 0)
   {
      // --- We are running in debug mode.
      //     This is a bit ugly but should work fine
      //     for debugging purposes.
      DEBUG_MODE = true;

      // A console server is what a debugger or a coverage tool launches, and
      // the regression suite drives the server over COM. Until 11 September
      // 2026 a client's CreateObject("hMailServer.Application") could not
      // reach it: RegisterAppID above had just re-asserted the AppID's
      // LocalService value, so the COM service control manager started the
      // service instead of connecting to this process - and this branch
      // registered no class objects for it to connect to anyway. So, here
      // only: the LocalService value comes off the AppID for the life of this
      // process, the class objects are registered before the database is
      // opened and resumed after it, in the order ServiceMain uses, and the
      // module does not unload on a client's last release, as the service does
      // not. The service must be stopped first; the two would share the ports.
      _AtlModule.run_as_service_ = true;
      _AtlModule.UnregisterLocalService();
      _AtlModule.RegisterObjects();

      // Connect to the database and create configuration objects.
      InitializeApplication();

      // Inform the OLE SCM that it can now create objects.
      _AtlModule.ResumeObjects();

      // The process is a Windows-subsystem one, so there is no console to
      // close: it ends when a WM_QUIT reaches this thread - posted by whoever
      // launched it (a coverage run does, once the suite is over) - and stops
      // the servers and closes the database on the way out, as a service stop
      // does.
      MSG msg;
      while (GetMessage(&msg, 0, 0, 0) > 0)
      {
         TranslateMessage(&msg);
         DispatchMessage(&msg);
      }

      if (!HM::Application::Instance()->ShutDown())
      {
         fflush(nullptr);
         TerminateProcess(GetCurrentProcess(), 0);
      }
   }
   else if (sLastParam.CompareNoCase(_T("--upgrade-database")) == 0)
   {
      // See UpgradeDatabaseFromCommandLine. Stop the service first when it is
      // running against the same database: the walk takes each step in a
      // transaction on its own connection, and a running server would hold
      // a schema it does not know.
      iRet = UpgradeDatabaseFromCommandLine();
   }
   else if (sLastParam.CompareNoCase(_T("/GrantServiceAccess")) == 0)
   {
      // What the service's own identity, NT SERVICE\hMailServer, needs in order
      // to run as something less than LocalSystem: Modify on the program
      // folder's own Data, Logs, Temp and Database folders and on the folders
      // an administrator approved, Read on the program's own folders - and nothing a
      // configuration the service can write names (Common/Util/
      // ServiceAccount.h, docs/ServiceAccount.md). Run elevated by the installer
      // and by the Control Panel's button; beside a running service as well as
      // a stopped one, since it changes permissions and nothing the service
      // holds. One line per grant goes where the other verbs' lines go. The
      // exit codes are ServiceAccount::RunGrantCommand's.
      AttachVerbOutput();

      HM::ServiceAccount::GrantOptions grantOptions;
      HM::String grantOptionsWhy;
      if (!ReadGrantOptions(grantOptions, grantOptionsWhy))
      {
         WriteVerbError(grantOptionsWhy);
         iRet = 1;
      }
      else
         iRet = HM::ServiceAccount::RunGrantCommand(grantOptions);
   }
   else if (sLastParam.CompareNoCase(_T("/ProtectFolders")) == 0)
   {
      // The program folder's own Data, Logs, Temp, Database and Events folders
      // and hMailServer.INI made private to SYSTEM, the Administrators and the
      // service, whatever account it runs as: until 6.3.6 they inherited
      // BUILTIN\Users' Read from Program Files, and every user of the computer
      // could read the mail, the logs and the database's password (Common/Util/
      // ServiceAccount.h, docs/ServiceAccount.md). Run elevated by the installer
      // at every installation and upgrade; beside a running service as well as a
      // stopped one. The exit codes are ServiceAccount::RunProtectCommand's.
      AttachVerbOutput();

      HM::ServiceAccount::ProtectOptions protectOptions;
      int count = 0;
      LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
      for (int index = 1; arguments != nullptr && index < count; index++)
      {
         if (HM::String(arguments[index]).CompareNoCase(_T("--report-only")) == 0)
            protectOptions.reportOnly = true;
      }
      if (arguments != nullptr)
         LocalFree(arguments);

      iRet = HM::ServiceAccount::RunProtectCommand(protectOptions);
   }
   else if (sLastParam.CompareNoCase(_T("--grant-database-login")) == 0)
   {
      // What NT SERVICE\hMailServer needs to open its database when that is SQL
      // Server reached with Windows authentication on this computer - a login, a
      // user in the database and db_owner - made over this process's own
      // connection, as the account that runs it (ServiceAccount::
      // RunDatabaseLoginCommand). Run elevated by the installer before it moves
      // the service to the virtual account, and by DBSetup when it finishes a new
      // installation. Beside a running service as well as a stopped one: it
      // changes SQL Server's principals, and nothing the service holds.
      AttachVerbOutput();
      iRet = HM::ServiceAccount::RunDatabaseLoginCommand();
   }
   else if (sLastParam.CompareNoCase(_T("/SetAdministratorPassword")) == 0)
   {
      // See SetAdministratorPasswordFromInput.
      AttachVerbOutput();
      iRet = SetAdministratorPasswordFromInput();
   }
   else if (sLastParam.CompareNoCase(_T("--create-stored-secrets-key")) == 0)
   {
      // See CreateStoredSecretsKeyFromCommandLine. Not refused while the service
      // runs: it writes the file and the database's record, and the service takes
      // the key at its next start or reinitialise.
      iRet = CreateStoredSecretsKeyFromCommandLine();
   }
   else if (FindVerbWithArgument(L"--forget-sealed-secrets", secretsKeyFile))
   {
      // See ForgetSealedSecretsFromCommandLine: the argument is the lost key's id.
      // Refused while the service runs.
      iRet = ForgetSealedSecretsFromCommandLine(secretsKeyFile);
   }
   else if (FindVerbWithArgument(L"--export-stored-secrets-key", secretsKeyFile))
   {
      // See ExportStoredSecretsKeyFromCommandLine. Not refused while the service
      // runs: it reads the key, and makes none.
      iRet = ExportStoredSecretsKeyFromCommandLine(secretsKeyFile);
   }
   else if (FindVerbWithArgument(L"--import-stored-secrets-key", secretsKeyFile))
   {
      // See ImportStoredSecretsKeyFromCommandLine. The running service takes the
      // key at its next start or reinitialise, which the last line says.
      iRet = ImportStoredSecretsKeyFromCommandLine(secretsKeyFile);
   }
   else if (sLastParam.CompareNoCase(_T("--set-admin-password")) == 0)
   {
      // See SetAdministratorPasswordFromCommandLine: the password from standard
      // input or HM_ADMIN_PASSWORD, never from the command line. Not refused while
      // the service runs, as on Linux: it writes the file, and the service takes
      // the password at its next start or reinitialise.
      iRet = SetAdministratorPasswordFromCommandLine();
   }
   else if (sLastParam.CompareNoCase(_T("--apply-environment")) == 0)
   {
      // Every HM_SETTING_* and HM_PROPERTY_* variable of this process's
      // environment stored, as `hmailserver --apply-environment` stores them on
      // Linux, through the same unit (Common/Application/EnvironmentSettings;
      // docs/EnvironmentConfiguration.md). Not refused while the service runs,
      // as the upgrade is: it changes settings, not the schema, and the service
      // takes them at its next reinitialise or start, which the last line says.
      // The lines go to the handles the caller gave - a script's or a test's
      // redirected output - or, when there are none, to the console the caller
      // has, as the upgrade's do.
      if (GetStdHandle(STD_OUTPUT_HANDLE) == NULL && AttachConsole(ATTACH_PARENT_PROCESS))
      {
         HANDLE console = CreateFile(_T("CONOUT$"), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
         if (console != INVALID_HANDLE_VALUE)
         {
            SetStdHandle(STD_OUTPUT_HANDLE, console);
            SetStdHandle(STD_ERROR_HANDLE, console);
         }
      }

      iRet = HM::EnvironmentSettings::RunCommand();
   }
   else if (newInstallDefaultsVerb)
   {
      // What a new installation starts with: a self-signed certificate, STARTTLS
      // offered on the four listeners, and a password refused without TLS from
      // any address but this computer's. The installer runs it for a database it
      // has just made, with the service stopped, before it starts the service;
      // it changes a server only while that server is exactly as a new database
      // leaves it (exit 5 otherwise). The lines go where the profile verb's go.
      //
      // Refused while the service runs, as the profile verb is: a running server
      // holds the listeners and the ranges it loaded, and would go on serving
      // them without TLS until a restart. Exit 4.
      AttachVerbOutput();

      if (HMailServerServiceIsRunning())
      {
         WriteVerbError("The hMailServer service is running. Stop it (Stop-Service hMailServer) to apply a new installation's defaults, then start it again. Nothing was changed.\r\n");
         iRet = 4;
      }
      else
         iRet = HM::NewInstallDefaults::RunCommand(false, String());
   }
   else if (profileVerb || profileRevert)
   {
      // A setting profile applied or put back (Common/Application/Profiles;
      // docs/Profiles.md), as `hmailserver --apply-profile` and `--revert-profile`
      // do it on Linux: the diff, the way back, then the writes, one line each. The
      // installer's last page runs it for a new installation, with the service
      // stopped, --once so that a second run of the installer leaves a server that
      // has had a profile alone, and --if-new so that a database already in service
      // is never changed without its diff. The lines go where --apply-environment's
      // go.
      //
      // Refused while the service runs, as --upgrade-database is: a running server
      // holds the settings it loaded and would not see these until a reinitialise,
      // and it is changed through REST, COM, the Deck or the Control Panel, which
      // show the diff first. Exit 4.
      AttachVerbOutput();

      if (HMailServerServiceIsRunning())
      {
         WriteVerbError("The hMailServer service is running. A running server is changed through the Control Deck, the Control Panel, REST or COM, which show the diff first; stop the service (Stop-Service hMailServer) to use the command line. Nothing was changed.\r\n");
         iRet = 4;
      }
      else if (profileRevert)
         iRet = HM::Profiles::RunRevertCommand();
      else if (profileName.IsEmpty() || profileName.Left(2) == _T("--"))
      {
         WriteVerbError("--apply-profile needs the profile's name: office, gateway, relay or sender, or none to record that no profile is wanted. Add --confirm to apply an entry that closes a listener, trusts an address range, turns a filter or a check off or weakens it, or shortens a window of days; --once to leave a server something was recorded for as it is; and --if-new to leave a server in service as it is.\r\n");
         iRet = 2;
      }
      else
         iRet = HM::Profiles::RunCommand(profileName, profileConfirmed, profileOnce, profileIfNew);
   }
   else if (storeVerb != 0)
   {
      // The lines go where the profile verb's go. The rewrites are refused while
      // the service runs, as the upgrade and the profiles are: a running server
      // reads and writes the files a walk renames, and counts the blobs a walk
      // counts, in a process the walk cannot lock. Measuring changes nothing and
      // runs beside it. Exit 4.
      AttachVerbOutput();

      if (storeVerb != HM::MessageStoreMaintenance::VerbMeasure && HMailServerServiceIsRunning())
      {
         WriteVerbError("The hMailServer service is running. Stop it (Stop-Service hMailServer) to rewrite the message store or recount its blobs, then start it again; --measure-store runs beside it. Nothing was changed.\r\n");
         iRet = 4;
      }
      else if (storeAccountGiven && (storeAccount.IsEmpty() || storeAccount.Left(2) == _T("--")))
      {
         WriteVerbError("--account needs the account's address; for example --account alice@example.com.\r\n");
         iRet = 2;
      }
      else if (storeDomainGiven && (storeDomain.IsEmpty() || storeDomain.Left(2) == _T("--")))
      {
         WriteVerbError("--domain needs the domain's name; for example --domain example.com.\r\n");
         iRet = 2;
      }
      else if (storeAccountGiven && storeDomainGiven)
      {
         WriteVerbError("--account and --domain each say what the walk covers; give one of them, or neither for the whole store.\r\n");
         iRet = 2;
      }
      else
      {
         // Every walk first finds no link in the data folder that the
         // administrators do not alone control, and holds it while it runs
         // (DataFolderHoldsNoLink). Exit 1. --measure-store too since 6.3.6: it
         // changes nothing and says what it read only to whoever runs it, but it
         // reads every file with an administrator's rights, and one rule for every
         // walk is one nobody has to reason about again.
         HM::HeldFolder heldData;
         HM::String linkWhy;
         if (!DataFolderHoldsNoLink(heldData, linkWhy))
         {
            WriteVerbError(_T("Nothing was changed: ") + linkWhy + _T("\r\n"));
            iRet = 1;
         }
         else
            iRet = HM::MessageStoreMaintenance::RunCommand((HM::MessageStoreMaintenance::Verb) storeVerb, storeAccount, storeDomain);
      }
   }
   else if (checkDatabaseVerb)
   {
      // Runs beside the service: one connection of its own, opened and closed, and
      // nothing changed. With --config the file named is read instead of the
      // installed one, and this process logs where that file says - the crash
      // oracle read the installed file's log directory before the line was parsed.
      // The lines go where the profile verb's go. Exit 0 connected, 1 not, 2 not
      // configured.
      AttachVerbOutput();

      if (configGiven && checkDatabaseConfig.IsEmpty())
      {
         WriteVerbError("--config needs the configuration file's full path after it, as --config C:\\hMailServer\\check.ini. Nothing was checked.\r\n");
         iRet = 2;
      }
      else if (!checkDatabaseConfig.IsEmpty() && !FileUtilities::IsFullPath(checkDatabaseConfig))
      {
         WriteVerbError("--config needs the configuration file's full path, for example --config C:\\hMailServer\\check.ini.\r\n");
         iRet = 2;
      }
      else if (!checkDatabaseConfig.IsEmpty() && !FileUtilities::Exists(checkDatabaseConfig))
      {
         WriteVerbError("--config names a file that does not exist. Nothing was checked.\r\n");
         iRet = 2;
      }
      else
      {
         if (!checkDatabaseConfig.IsEmpty())
         {
            HM::IniFileSettings::SetInitializationFile(checkDatabaseConfig);
            HM::IniFileSettings::Instance()->ForgetLogDirectory();
         }

         iRet = HM::DatabaseTls::RunCheckCommand();
      }
   }
   else if (copyToObjects || copyFromObjects)
   {
      // The copy runs beside the service; with --remove-source it changes what
      // a running server reads, and is refused while the service runs. Exit 4.
      AttachVerbOutput();

      if (copyToObjects && copyFromObjects)
      {
         WriteVerbError("--copy-store-to-object-storage and --copy-store-from-object-storage go opposite ways; give one of them.\r\n");
         iRet = 2;
      }
      else if (removeSource && HMailServerServiceIsRunning())
      {
         WriteVerbError("The hMailServer service is running. Stop it (Stop-Service hMailServer) to copy with --remove-source; the copy without it runs beside the service. Nothing was changed.\r\n");
         iRet = 4;
      }
      else if (storeAccountGiven && (storeAccount.IsEmpty() || storeAccount.Left(2) == _T("--")))
      {
         WriteVerbError("--account needs the account's address; for example --account alice@example.com.\r\n");
         iRet = 2;
      }
      else if (storeDomainGiven && (storeDomain.IsEmpty() || storeDomain.Left(2) == _T("--")))
      {
         WriteVerbError("--domain needs the domain's name; for example --domain example.com.\r\n");
         iRet = 2;
      }
      else if (storeAccountGiven && storeDomainGiven)
      {
         WriteVerbError("--account and --domain each say what the copy covers; give one of them, or neither for the whole store.\r\n");
         iRet = 2;
      }
      else
      {
         // Every copy first finds no link in the data folder (DataFolderHoldsNoLink).
         // Exit 1. One into it, or one that removes from it, would write or delete
         // through a link; and one to the object store alone would read, with an
         // administrator's rights, whatever a link led to and hand its bytes to a
         // store the service's account reads - until the re-check of 6.3.6 that
         // copy was let through as one that only reads.
         HM::HeldFolder heldData;
         HM::String linkWhy;
         if (!DataFolderHoldsNoLink(heldData, linkWhy))
         {
            WriteVerbError(_T("Nothing was changed: ") + linkWhy + _T("\r\n"));
            iRet = 1;
         }
         else
            iRet = HM::MessageStoreObjects::RunCopyCommand(copyToObjects, storeAccount, storeDomain, removeSource);
      }
   }
   else if (sLastParam.CompareNoCase(_T("RunAsService")) == 0)
   {
      _AtlModule.run_as_service_ = true;

     iRet = _AtlModule.WinMain(nShowCmd);
     
     if (iRet != 0)
     {
        ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5601, "_tWinMain", Formatter::Format("_AtlModule.WinMain returned {0}", iRet));
     }
   }

   //_CrtDumpMemoryLeaks();
   
   return iRet;
}


DWORD WINAPI StartServiceInitialization(LPVOID vd)
{

   SERVICE_TABLE_ENTRY   DispatchTable[] = 
   { 
      { _T("hMailServer"), ServiceMain }, 
      { NULL,          NULL        } 
   }; 

   StartServiceCtrlDispatcher( DispatchTable );

   return 0;
}

void
ReportServiceStatus(DWORD dwState, DWORD dwControlsAccepted)
{
   ServiceStatus.dwCurrentState  = dwState;
   ServiceStatus.dwControlsAccepted   = dwControlsAccepted;

   // Send the server status to SCM so that 
   // it knows that we have started.
   SetServiceStatus (ServiceStatusHandle,  &ServiceStatus);
}

static void WINAPI
ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv)
//------------------------------------------------------------------------//
// The main entry point for SCM.
//------------------------------------------------------------------------//
{
   // MSDN: 
   // The ServiceMain function of a new service should immediately call the RegisterServiceCtrlHandler 
   // function to register a control handler function with the control dispatcher.
   ServiceStatusHandle = RegisterServiceCtrlHandler(_T("hMailServer"), ServiceController); 

   // Then it immediately calls the SetServiceStatus function to notify the 
   // service control manager that its status is SERVICE_START_PENDING. 
   // Tell SCM that we have started.
   ReportServiceStatus(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

   // In Windows 2000/XP, we need to register objects directly
   // after service start. Otherwise it won't work. In later
   // versions, we must register them after the service has
   // notified the SCM about the status (which is right about now). 
   if (SystemInformation::GetOperatingSystem() == SystemInformation::Windows2003)
      _AtlModule.RegisterObjects();

   // The arguments of the start, which the service controller passes here and
   // not on the command line: none from net start, services.msc, a boot or a
   // COM activation, and --for-database-upgrade from the installer's start
   // for the database step of an upgrade (StartServiceForDatabaseUpgrade in
   // hMailServerInnoExtension.iss, "sc start hMailServer --for-database-upgrade").
   // That start expects a database older than this binary, and the next
   // OnDatabaseConnected refuses it without reporting HM5011
   // (Application::SetStartedForDatabaseUpgrade). Every entry is read,
   // the first included, because whether the controller puts the service's
   // name first is not something to rely on.
   for (DWORD index = 0; lpszArgv != nullptr && index < dwArgc; index++)
   {
      if (lpszArgv[index] == nullptr)
         continue;

      String argument = lpszArgv[index];
      if (argument.CompareNoCase(_T("--for-database-upgrade")) == 0)
         HM::Application::Instance()->SetStartedForDatabaseUpgrade();
      else if (argument.CompareNoCase(_T("--apply-new-install-defaults")) == 0)
         g_finishNewInstallationAtStart = true;
   }

   // Connect to the database and create configuration objects.
   // We need to do this AFTER we have registered the COM objects
   // Otherwise it won't work properly "on all" Win2k3 machines.
   InitializeApplication();

   
   // Inform the OLE SCM that it can now create objects.
   // based on this application. This needs to done after
   // we have connected to the database (Otherwise clients
   // may create instances of for example hMailServer.Database
   // with no open connection.
   _AtlModule.ResumeObjects();
}

void
TerminateServer()
{
   // Tell the crash oracle that this is an orderly stop. An exception thrown by a
   // thread being torn down is still recorded and still reported, but it no longer
   // gets escalated to a hard process kill that would race the stop notification
   // below and make Windows report a service failure. A memory-safety fault is
   // treated as fatal either way - that is a defect whenever it happens.
   HM::CrashOracle::NotifyShutdownStarted();

   // The stop now waits up to ShutdownDrainSeconds for the sessions and the
   // deliveries in flight, with the listeners closed. The service controller has
   // to be told how long that may take - a wait hint of zero, which this status
   // carried until 23 September 2026, is the controller's default, after which
   // Stop-Service, services.msc and a Windows shutdown report error 1053 while
   // the drain is still honouring its window - and told, about once a second,
   // that the stop is still progressing (the checkpoint), which the drain's
   // wait loop does through the callback. The ninety seconds on top are the
   // installer's own margin for the same wait (GetServiceStopBudgetMs in
   // hMailServerInnoExtension.iss): the minute the stop gives the message
   // indexer's thread, and thirty seconds of headroom. On SQL Server Compact
   // up to twelve seconds of that headroom go on a database connection still
   // in use when the stop closes the database, which is waited for up to that
   // long, or until it is handed back
   // (DatabaseConnectionManager::SecondsToWaitForConnectionsInUse).
   ServiceStatus.dwWaitHint = (HM::IniFileSettings::Instance()->GetShutdownDrainSeconds() + 90) * 1000;
   ServiceStatus.dwCheckPoint++;
   ReportServiceStatus(SERVICE_STOP_PENDING, 0);

   HM::Application::Instance()->SetStopProgressCallback([]()
   {
      ServiceStatus.dwCheckPoint++;
      if (ServiceStatusHandle != 0)
         SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
   });

   // The process ends after it: a stop, a start or a reinitialise under way is
   // waited for - this service's own start among them, which reports the
   // service running before it opens the database (ServiceMain,
   // InitializeApplication) - the message indexer a minute at most, the
   // checkpoint advancing meanwhile, and the database closed but for a
   // connection still in use, SQL Server Compact's flush interval waited out
   // for one (Application::ShutDown).
   const bool ended = HM::Application::Instance()->ShutDown();

   HM::Application::Instance()->SetStopProgressCallback(std::function<void()>());

   if (ServiceStatusHandle != 0)
   {
      DWORD MainThread = GetCurrentThreadId();
      PostThreadMessage(MainThread, WM_QUIT, 0, 0);

      ReportServiceStatus(SERVICE_STOPPED, 0);
   }

   // A thread the stop gave up on - the indexer's, a work queue's, or a
   // reinitialise waiting for the indexer - would run on through the
   // destructors of statics as the process ends, and a connection still in use
   // was left open for its caller: the process ends here instead, once the
   // service controller has been told, the thread with it. The end of the
   // process does not close what was left open the way a close does: a
   // database server rolls back the statement it cuts off, and SQL Server
   // Compact, which runs in this process, keeps only what it wrote to its file
   // - which ShutDown has waited for. Until the re-check of 10 October 2026
   // this comment said that the end closed the connections.
   if (!ended)
   {
      fflush(nullptr);
      TerminateProcess(GetCurrentProcess(), 0);
   }
}


static void WINAPI 
ServiceController (DWORD Opcode) 
//---------------------------------------------------------------------------()
// DESCRIPTION:
// Entrypoint for SCM to change status of this service.
//---------------------------------------------------------------------------()
{ 

   switch(Opcode) 
   { 
   case SERVICE_CONTROL_PAUSE: 
      ServiceStatus.dwCurrentState = SERVICE_PAUSED; 
      break; 
   case SERVICE_CONTROL_CONTINUE: 
      ServiceStatus.dwCurrentState = SERVICE_RUNNING; 
      break; 
   case SERVICE_CONTROL_SHUTDOWN:
      LOG_DEBUG("Received shutdown-request from Windows.");
      TerminateServer();
      break;
   case SERVICE_CONTROL_STOP: 
      LOG_DEBUG("Received stop-request from Windows.");
      TerminateServer();
      // --- Send message to the main thread to quit.
      return; 
   case SERVICE_SUBMIT_MAIL:
      // Submit pending e-mails.
      HM::Application::Instance()->SubmitPendingEmail();
      break;

   } 

   // Send current status. 
   SetServiceStatus (ServiceStatusHandle,  &ServiceStatus);

   return; 
} 

// A start asked for with --apply-new-install-defaults ("sc start hMailServer
// --apply-new-install-defaults"), with the database open and the listeners not
// yet started: a new installation's defaults applied by the service
// (NewInstallDefaults::ApplyAtServiceStart), and its stored-secrets key made
// (StoredSecrets::CreateKey), as hMailServer.exe --apply-new-install-defaults and
// --create-stored-secrets-key do them. For the installation whose administrator
// cannot open its database - SQL Server with Windows authentication, where
// LocalSystem is a SQL Server administrator and the person running Setup is
// not - so that both verbs, run as that person, failed: Setup and DBSetup then
// start the service so, and an administrator can (docs/ServiceAccount.md). The
// key is written into hMailServer.ini, which a service running as LocalSystem
// may write and NT SERVICE\hMailServer may not; the line says which. True when
// the server may have been changed, and the start is then made again
// (Application::Reinitialize) so that the listeners are built from what it now
// holds. Called with the database open and the servers' lock held
// (Application::InitInstanceAndStart), so that a stop meanwhile waits for it.
static bool FinishNewInstallationAtStart()
{
   const int defaults = HM::NewInstallDefaults::ApplyAtServiceStart();

   // The key only for the new installation this start found and applied the
   // defaults to (0, or 1 when part of them could not be written): a server in
   // service started with the argument gets no key that way - an administrator
   // makes one for it with --create-stored-secrets-key.
   if (defaults != 0 && defaults != 1)
   {
      LOG_APPLICATION(_T("New installation: no stored-secrets key is made at this start, since its database is not a new installation's whose defaults it applied."));
      return false;
   }

   HM::AnsiString keyId;
   bool alreadyHeld = false;
   HM::String problem;
   bool keyMade = false;
   if (HM::StoredSecrets::CreateKey(keyId, alreadyHeld, problem))
   {
      keyMade = !alreadyHeld;
      LOG_APPLICATION(alreadyHeld
         ? HM::String(_T("New installation: this server already holds the database's stored-secrets key ")) + HM::String(keyId) + _T(", and none was made.")
         : HM::String(_T("New installation: the service made the stored-secrets key ")) + HM::String(keyId) +
           _T(", wrote it into hMailServer.ini under this machine's DPAPI and recorded it in the database. Export it now - hMailServer.exe --export-stored-secrets-key FILE, from an elevated prompt - and keep the file with the database backups, stored apart from them."));
   }
   else
   {
      HM::ErrorManager::Instance()->ReportError(HM::ErrorManager::Medium, 7498, "FinishNewInstallationAtStart",
         _T("New installation: the stored-secrets key was not made at this start: ") + problem +
         _T(". hMailServer.exe --create-stored-secrets-key, from an elevated prompt by an account that can open the database, or this start again on LocalSystem, makes it."));
   }

   return defaults == 0 || defaults == 1 || keyMade;
}

void InitializeApplication()
{
   // Initialize the application.
   // This will cause the application object to be created,
   // and the settings to be loaded. If the settings are
   // loaded properly, StartServers() will start the TCP/IP
   // servers.

   // First allocate all singletons. A start asked to finish a new installation
   // (FinishNewInstallationAtStart) does that once the database is open, and
   // starts again, listeners and all, when it changed the server. All of it
   // holds the servers' lock (Application::InitInstanceAndStart): the service
   // has told the service controller that it runs and takes a stop - the
   // service controller's, a Windows shutdown, sc start followed by sc stop,
   // or the installer's after a database step that could not reach the
   // service; its ordinary stops come after a COM call, which waits for
   // ServiceMain's ResumeObjects, after this - which until 10 October 2026
   // closed the database under InitInstance, or under the new installation's
   // defaults, the stored-secrets key and the ini they write, and now waits
   // for them.
   const bool finish = g_finishNewInstallationAtStart;
   g_finishNewInstallationAtStart = false;

   String sErrorMessage;
   HM::Application::Instance()->InitInstanceAndStart(sErrorMessage,
      finish ? std::function<bool()>(&FinishNewInstallationAtStart) : std::function<bool()>());

   // Now that the configuration has been loaded, the logger has a log mask and this
   // line will actually reach the application log. Said once per process start, so
   // that an operator reading a log can tell whether crash records are being kept
   // and where they are.
   HM::CrashOracle::LogInstallationStatus();
}


