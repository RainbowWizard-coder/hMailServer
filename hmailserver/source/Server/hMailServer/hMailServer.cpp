// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
// START: Enable tracking of memory leaks.

#ifdef _DEBUG
   #define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
   #define new DEBUG_NEW
#endif

#include "resource.h"
#include "hMailServer.h"

#include "../Common/Util/ServiceManager.h"
// For the LoadSettings call in the /Register branch: the service account the
// registration installs is read from here, and nothing else on that path loads it.
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Application/EnvironmentSettings.h"
#include "../Common/Application/Profiles.h"
#include "../Common/Application/WindowsEventLog.h"
#include "../Common/Persistence/MessageStoreMaintenance.h"
#include "../Common/Util/ClassTester.h"
#include "../Common/Util/SystemInformation.h"
#include "../Common/Util/CrashOracle.h"
#include "../Common/Persistence/SchemaUpgrader.h"
#include "../Common/SQL/DALConnection.h"
#include "../Common/SQL/DALConnectionFactory.h"
#include "../Common/SQL/DatabaseSettings.h"

#include <cstdio>

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

static int UpgradeDatabaseFromCommandLine()
{
   if (AttachConsole(ATTACH_PARENT_PROCESS))
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


extern "C" int WINAPI _tWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, 
                                LPTSTR lpCmdLine, int nShowCmd)
{
   // Arm the crash oracle before anything else runs. Both of its hooks are
   // process-wide, so installing them here covers every thread the server creates
   // later, including the ones the service control manager and boost::asio start on
   // our behalf. It touches nothing but the ini file, so it is safe this early -
   // and it has to be this early, because a fault during startup is exactly the kind
   // that otherwise disappears.
   HM::CrashOracle::Install();

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
      // ---  Unregister the hMailServer service.
      HM::ServiceManager hSCM;
      hSCM.UnregisterService("hMailServer");

      _AtlModule.UnregisterServer(TRUE);
      _AtlModule.UnregisterAppId();

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
      /*
         The settings have to be loaded before the service is registered, and
         nothing on this path had loaded them.

         ServiceManager::RegisterService and ReconfigureService_ both call
         IniFileSettings::GetServiceAccountName() / GetServiceAccountPassword()
         to decide which account the service logs on as. Those fields are
         assigned in exactly one place - IniFileSettings::LoadSettings - and the
         only caller of it is Application::InitInstance, which this branch never
         reaches: /Register also sets bRegisterTypelib, so control returns a few
         lines below.

         The result was that ServiceAccountName had never worked, by any route.
         The registration read a default-constructed empty String, passed NULL
         for lpServiceStartName, and CreateService therefore installed the
         service as LocalSystem while ChangeServiceConfig was told "no change" -
         so an administrator could set the value, run the documented command any
         number of times, and the account would never move. There was no error,
         because from the SCM's point of view nothing was asked for.

         LoadSettings is safe here. It reads hMailServer.ini and nothing else -
         GetInitializationFile derives the path from the binary's own directory -
         and Application::InitInstance itself calls it before OpenDatabase, so
         it does not require a database, a COM session or a started server.
      */
      HM::IniFileSettings::Instance()->LoadSettings();

      // --- Register the hMailServer service.
      HM::ServiceManager hSCM;
      if (!hSCM.RegisterService("hMailServer", "hMailServer"))
      {
         return -1;
      }
   }

   if (bRegisterTypelib)
   {
      if (FAILED(_AtlModule.RegisterAppId()))
      {
         return -1;
      }
      if (FAILED(_AtlModule.RegisterServer(TRUE)))
      {
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

      HM::Application::Instance()->StopServers();

      HM::Application::Instance()->ExitInstance();
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

      HM::Application::Instance()->StopServers();
      HM::Application::Instance()->ExitInstance();
   }
   else if (sLastParam.CompareNoCase(_T("--upgrade-database")) == 0)
   {
      // See UpgradeDatabaseFromCommandLine. Stop the service first when it is
      // running against the same database: the walk takes each step in a
      // transaction on its own connection, and a running server would hold
      // a schema it does not know.
      iRet = UpgradeDatabaseFromCommandLine();
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
         iRet = HM::MessageStoreMaintenance::RunCommand((HM::MessageStoreMaintenance::Verb) storeVerb, storeAccount, storeDomain);
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
   // wait loop does through the callback. The thirty seconds on top is the
   // installer's own margin for the same wait.
   ServiceStatus.dwWaitHint = (HM::IniFileSettings::Instance()->GetShutdownDrainSeconds() + 30) * 1000;
   ServiceStatus.dwCheckPoint++;
   ReportServiceStatus(SERVICE_STOP_PENDING, 0);

   HM::Application::Instance()->SetStopProgressCallback([]()
   {
      ServiceStatus.dwCheckPoint++;
      if (ServiceStatusHandle != 0)
         SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
   });

   HM::Application::Instance()->StopServers();

   HM::Application::Instance()->SetStopProgressCallback(std::function<void()>());

   HM::Application::Instance()->ExitInstance();

   if (ServiceStatusHandle != 0)
   {
      DWORD MainThread = GetCurrentThreadId();
      PostThreadMessage(MainThread, WM_QUIT, 0, 0);

      ReportServiceStatus(SERVICE_STOPPED, 0);
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

void InitializeApplication()
{
   // Initialize the application.
   // This will cause the application object to be created,
   // and the settings to be loaded. If the settings are
   // loaded properly, StartServers() will start the TCP/IP
   // servers.

   // First allocate all singletons.
   String sErrorMessage;
   if (HM::Application::Instance()->InitInstance(sErrorMessage))
   {
      // We were able to connect to the database server
      // and the database is of the correct version. So
      // now we should start the TCP/IP-servers.
      HM::Application::Instance()->StartServers();
   }

   // Now that the configuration has been loaded, the logger has a log mask and this
   // line will actually reach the application log. Said once per process start, so
   // that an operator reading a log can tell whether crash records are being kept
   // and where they are.
   HM::CrashOracle::LogInstallationStatus();
}


