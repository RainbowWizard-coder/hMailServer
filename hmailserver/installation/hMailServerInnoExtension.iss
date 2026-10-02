// The database schema version this build needs, REQUIRED_DB_VERSION in the
// server's Constants.h, read when the installer is compiled - the number the new
// server and its DBUpdater upgrade a database to, and which
// build\check-schema-versions.ps1 holds DBUpdater's chain and the scripts to. A
// database already at it, or past it, is one the database step cannot move:
// DBUpdater has no step to run on it, and the service's own start with [Database]
// UpgradeOnStart=1 none either (EndDatabaseStep). The compile stops when it
// cannot be read. The #sub assigns with #expr: a #define inside it would make a
// variable of its own, gone when it returns.
#define HM_CONSTANTS_FILE AddBackslash(SourcePath) + "..\source\Server\Common\Application\Constants.h"
#define HM_REQUIRED_DB_VERSION ""
#define HmConstantsHandle 0
#define HmConstantsLine ""
#sub HmReadConstantsLine
  #expr HmConstantsLine = Trim(FileRead(HmConstantsHandle))
  #if Pos("#define REQUIRED_DB_VERSION ", HmConstantsLine) == 1
    #expr HM_REQUIRED_DB_VERSION = Trim(Copy(HmConstantsLine, Len("#define REQUIRED_DB_VERSION ") + 1))
  #endif
#endsub
#for {HmConstantsHandle = FileOpen(HM_CONSTANTS_FILE); HmConstantsHandle && !FileEof(HmConstantsHandle); 0} HmReadConstantsLine
#if HmConstantsHandle
  #expr FileClose(HmConstantsHandle)
#endif
#if HM_REQUIRED_DB_VERSION == "" || Int(HM_REQUIRED_DB_VERSION, 0) < 6000
  #error REQUIRED_DB_VERSION could not be read from Constants.h
#endif

[code]
// GLOBAL
var
  g_pageAccessKey: TInputQueryWizardPage;
  g_szAdminPassword: String;
  g_szUpgradeToken: String;
  // True when the token came in HM_UPGRADE_TOKEN, which only hMailServer.Updater
  // sets - for an administrator's install from the Control Panel, and from 6.3.6
  // for the service's own live update too (docs/ServiceAccount.md, "The live
  // update"): it has ended every Control Panel running from the installation
  // already, so the check for one by its window title is not made
  // (InitializeSetup).
  g_bAdministratorUpdate: Boolean;
  
  g_pageDBType: TWizardPage;
  g_bUseInternal : Boolean;

  rdoUseInternal : TRadioButton;
  rdoUseExternal : TRadioButton;

  // The setting profile a new installation starts in (docs/Profiles.md): offered
  // on the last page before the installation, for a new installation only - one
  // with no database yet - and applied after the database is made, built-in or
  // external, with the service stopped, by hMailServer.exe --apply-profile NAME
  // --once --if-new --confirm.
  g_pageProfile: TInputOptionWizardPage;
  g_bNewDatabase: Boolean;
  g_bNewDatabaseKnown: Boolean;

  // The account the service runs as (docs/ServiceAccount.md), on a page of its
  // own: NT SERVICE\hMailServer, a virtual account, for a new installation; the
  // account it already has for an upgrade, with the way to the other. Passed to
  // hMailServer.exe --service-account virtual|localsystem|keep /Register, and
  // /serviceaccount= for a silent installation. The account the service had
  // before this installation touched it is read once, as the database type is.
  g_pageServiceAccount: TInputOptionWizardPage;
  g_szServiceAccountBefore: String;
  g_bServiceAccountKnown: Boolean;

  // Set when the service account could not be granted its folders on an
  // installation whose service already runs as something other than
  // LocalSystem - the one grant failure that fails the installation, and says
  // so in its own words rather than the database's.
  g_bGrantFailed: Boolean;
  g_bMssqlSentenceAdded: Boolean;

  // Set when a step after the files - the service account's folders, the
  // administrator password, the database or the service (RunPostInstallTasks) -
  // did not complete, so that setup ends with exit code 10 - or 11 once the
  // database has changed - rather than 0 (GetCustomSetupExitCode). The exception
  // CurStepChanged raises for it is
  // logged and shown, but Inno Setup carries on past an exception in an event
  // function and exits 0: on the installer smoke VM on 1 October 2026 an update
  // whose database upgrade failed (DBSetupQuick exit code 2) ended with 0, and
  // hMailServer.Updater reported it installed.
  g_bStepsIncomplete: Boolean;

  // The version hMailServer.Updater puts back when this installation does not
  // complete: HM_UPDATE_ROLLBACK_VERSION, which the Updater sets - from 6.3.6, for
  // the service's own live update and for an administrator's install alike - only
  // when it holds that version's installer, verified, to run (the rollback image),
  // and which is read only beside the token the Updater gives (g_bAdministratorUpdate).
  // Empty for an installation somebody runs by hand. While it is set, a step before
  // the database step that did not complete stops the installation there, before
  // anything has changed the database (RunPostInstallTasks): the Updater's rollback
  // is then made onto the database as that version left it, which is the only
  // database it can run on.
  g_szRollbackVersion: String;

  // The database's schema version - hm_dbversion, read through the hMailServer
  // service's own connection (ReadDatabaseVersion) - before this installation, from
  // the service it replaces, and after the database step; -1 where it could not be
  // read. g_bDatabaseChanged is set as the database step on an existing database
  // begins and cleared only when the two are both known and equal, or the version
  // before is known and one this build does not upgrade (EndDatabaseStep): from then on
  // the earlier version cannot run on the database, so setup never ends with 10,
  // on which hMailServer.Updater puts the earlier version back, but with 11
  // (GetCustomSetupExitCode).
  g_iDatabaseVersionBefore: Integer;
  g_iDatabaseVersionAfter: Integer;
  g_bDatabaseChanged: Boolean;

  // The external database a new installation is set up against without DBSetup's
  // wizard (docs/WindowsOnArm.md, "A new installation without a window"): given with
  // /dbtype, or HM_DB_TYPE in this process's environment, and read once, in
  // InitializeSetup, before anything changes the environment. DBSetup /unattended
  // sets it up (RunUnattendedDBSetup). Without it everything is as it was.
  g_bDatabaseGiven: Boolean;
  g_szDatabaseType: String;
  // Where it came from, for the setup log and the refusal: '/dbtype', or
  // 'HM_DB_TYPE in the environment' - which is also what a variable left set on
  // the machine for anything else is (docs/WindowsOnArm.md).
  g_szDatabaseFrom: String;

// The NT-service specific parts of the scrit below is taken
// from the innosetup extension knowledgebase.
// Author: Silvio Iaccarino silvio.iaccarino(at)de.adp.com
// Article created: 6 November 2002
// Article updated: 6 November 2002
// http://www13.brinkster.com/vincenzog/isxart.asp?idart=31

type
	SERVICE_STATUS = record
    	dwServiceType				: cardinal;
    	dwCurrentState				: cardinal;
    	dwControlsAccepted			: cardinal;
    	dwWin32ExitCode				: cardinal;
    	dwServiceSpecificExitCode	: cardinal;
    	dwCheckPoint				: cardinal;
    	dwWaitHint					: cardinal;
	end;
	// What QueryServiceStatusEx answers for SC_STATUS_PROCESS_INFO: SERVICE_STATUS
	// with the id of the service's process and its flags after it, nine DWORDs,
	// 36 bytes. The process id is how the stop below waits for the process itself
	// rather than for the service's word that it has stopped.
	SERVICE_STATUS_PROCESS = record
		dwServiceType				: cardinal;
		dwCurrentState				: cardinal;
		dwControlsAccepted			: cardinal;
		dwWin32ExitCode				: cardinal;
		dwServiceSpecificExitCode	: cardinal;
		dwCheckPoint				: cardinal;
		dwWaitHint					: cardinal;
		dwProcessId					: cardinal;
		dwServiceFlags				: cardinal;
	end;
	HANDLE = cardinal;
	
const
	SERVICE_QUERY_CONFIG		= $1;
	SERVICE_CHANGE_CONFIG		= $2;
	SERVICE_QUERY_STATUS		= $4;
	SERVICE_START				= $10;
	SERVICE_STOP				= $20;
	SERVICE_ALL_ACCESS			= $f01ff;
	SC_MANAGER_ALL_ACCESS		= $f003f;
	SERVICE_WIN32_OWN_PROCESS	= $10;
	SERVICE_WIN32_SHARE_PROCESS	= $20;
	SERVICE_WIN32				= $30;
	SERVICE_INTERACTIVE_PROCESS = $100;
	SERVICE_BOOT_START          = $0;
	SERVICE_SYSTEM_START        = $1;
	SERVICE_AUTO_START          = $2;
	SERVICE_DEMAND_START        = $3;
	SERVICE_DISABLED            = $4;
	SERVICE_DELETE              = $10000;
	SERVICE_CONTROL_STOP		= $1;
	SERVICE_CONTROL_PAUSE		= $2;
	SERVICE_CONTROL_CONTINUE	= $3;
	SERVICE_CONTROL_INTERROGATE = $4;
	SERVICE_STOPPED				= $1;
	SERVICE_START_PENDING       = $2;
	SERVICE_STOP_PENDING        = $3;
	SERVICE_RUNNING             = $4;
	SERVICE_CONTINUE_PENDING    = $5;
	SERVICE_PAUSE_PENDING       = $6;
	SERVICE_PAUSED              = $7;

	// For the wait on the service's process (StopHMailServerAndWaitForExit). The
	// Windows names are SC_STATUS_PROCESS_INFO, SYNCHRONIZE and WAIT_OBJECT_0; these
	// are named apart from them on purpose, because a constant Inno predefines
	// cannot be declared again - a duplicate identifier fails the compile, as
	// FILE_ATTRIBUTE_DIRECTORY did on 15 August 2026 (see below).
	SERVICE_STATUS_PROCESS_INFO = 0;
	SERVICE_STATUS_PROCESS_SIZE = 36;
	PROCESS_WAIT_ACCESS         = $00100000;
	PROCESS_WAIT_SIGNALLED      = 0;
	// How long the process may take to exit once the service has said it stopped.
	// About 1.1 seconds was measured on 25 September 2026; the bound is for the
	// process that does not go at all.
	PROCESS_EXIT_BUDGET_MS      = 30000;

  // Every dialog in this script is a SuppressibleMsgBox, and the reason is the
// 6.2.19 release: the database setup failed on a clean machine, the failure
// raised a plain MsgBox, and /VERYSILENT /SUPPRESSMSGBOXES only suppresses the
// SUPPRESSIBLE kind - so the silent install did not fail, it waited on a dialog
// with nobody at the keyboard, for ninety minutes, in CI, and would have waited
// forever on a real unattended deployment. Under /SUPPRESSMSGBOXES these now
// take their default button and the installer carries on to its exit code,
// which is what a script deploying it can actually read. Interactive installs
// see exactly the dialogs they always saw.

// BEGIN .NET INSTALLER	
  mdacURL = 'http://download.microsoft.com/download/4/a/a/4aafff19-9d21-4d35-ae81-02c48dcbbbff/MDAC_TYP.EXE';
  dotnet20URL = 'http://download.microsoft.com/download/5/6/7/567758a3-759e-473e-bf8f-52154438565a/dotnetfx.exe';
  // END .NET INSTALLER

  // FindFirst matches files as well as directories, and the .NET shared framework
  // marks a version with a directory - so DotNetDesktopMissing tests
  // FILE_ATTRIBUTE_DIRECTORY. That constant is NOT declared here: Inno Setup 6
  // predefines it (value $10) for its own FindFirst/TFindRec support, and a local
  // declaration is a duplicate-identifier compile error - which is exactly how the
  // 6.2.19 installer build failed on 2026-08-15, the first ISCC run since the
  // declaration was added.

  // Burn/msiexec exit codes that mean "installed", not "failed".
  ERROR_SUCCESS_REBOOT_REQUIRED = 3010;
  ERROR_PRODUCT_VERSION         = 1638;

  // The current download page for the runtime, quoted in the failure message.
  DOTNET_DOWNLOAD_URL = 'https://dotnet.microsoft.com/download/dotnet/{#DOTNET_CHANNEL}';

function ControlService(hService :HANDLE; dwControl :cardinal;var ServiceStatus :SERVICE_STATUS) : boolean;
external 'ControlService@advapi32.dll stdcall';		

function CloseServiceHandle(hSCObject :HANDLE): boolean;
external 'CloseServiceHandle@advapi32.dll stdcall';

function OpenService(hSCManager :HANDLE;lpServiceName: AnsiString; dwDesiredAccess :cardinal): HANDLE;
external 'OpenServiceA@advapi32.dll stdcall';

function OpenSCManager(lpMachineName, lpDatabaseName: AnsiString; dwDesiredAccess :cardinal): HANDLE;
external 'OpenSCManagerA@advapi32.dll stdcall';

function QueryServiceStatus(hService :HANDLE;var ServiceStatus :SERVICE_STATUS) : boolean;
external 'QueryServiceStatus@advapi32.dll stdcall';

// The service's process, opened and waited on (StopHMailServerAndWaitForExit).
// Named apart from the Windows functions for the reason the constants above are;
// BOOL parameters are passed as cardinal, which is their width.
function QueryServiceStatusProcess(hService: HANDLE; InfoLevel: cardinal; var Buffer: SERVICE_STATUS_PROCESS; cbBufSize: cardinal; var pcbBytesNeeded: cardinal): boolean;
external 'QueryServiceStatusEx@advapi32.dll stdcall';

function OpenProcessForWait(dwDesiredAccess: cardinal; bInheritHandle: cardinal; dwProcessId: cardinal): HANDLE;
external 'OpenProcess@kernel32.dll stdcall';

function WaitForProcessExit(hHandle: HANDLE; dwMilliseconds: cardinal): cardinal;
external 'WaitForSingleObject@kernel32.dll stdcall';

function CloseProcessForWait(hObject: HANDLE): boolean;
external 'CloseHandle@kernel32.dll stdcall';

// A variable of this process's environment, which a process Exec starts inherits:
// how the administrator password reaches hMailServer.exe --set-admin-password
// without being on its command line (WriteAdministratorPassword). A String reaches
// the W function as a pointer to its wide characters. Named apart from the API, so
// that it cannot meet an identifier Inno Setup predefines (as FILE_ATTRIBUTE_DIRECTORY
// did in 6.2.19).
function SetProcessEnvironmentVariable(lpName: String; lpValue: String): Boolean;
external 'SetEnvironmentVariableW@kernel32.dll stdcall';

// UpdateApplyToken's form: 64 lowercase hexadecimal characters.
function IsUpgradeTokenForm(Token: String): Boolean;
var
  i: Integer;
  c: Char;
begin
  Result := Length(Token) = 64;
  if not Result then
    Exit;
  for i := 1 to 64 do
  begin
    c := Token[i];
    if not (((c >= '0') and (c <= '9')) or ((c >= 'a') and (c <= 'f'))) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// A release's version as hMailServer.Updater passes it ("6.3.6", "6.3.7-rc1"): one
// to 40 letters, digits, dots and hyphens, beginning with a digit.
function IsReleaseVersionForm(Version: String): Boolean;
var
  i: Integer;
  c: Char;
begin
  Result := (Length(Version) > 0) and (Length(Version) <= 40) and (Version[1] >= '0') and (Version[1] <= '9');
  if not Result then
    Exit;
  for i := 1 to Length(Version) do
  begin
    c := Version[i];
    if not (((c >= '0') and (c <= '9')) or ((c >= 'a') and (c <= 'z')) or ((c >= 'A') and (c <= 'Z')) or (c = '.') or (c = '-')) then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// Ports 25, 110 and 143 in use by something other than hMailServer. A bind test
// over WinSock, which is what the ISC.DLL this replaced did: a 45 KB unsigned DLL
// with no version resource and no known upstream, shipped inside the installer
// for these fifteen lines. AnsiString parameters reach an external function as a
// pointer to their characters, which is how the WSADATA and sockaddr_in buffers
// are passed.
function WSAStartup(wVersionRequired: Word; lpWSAData: AnsiString): Integer;
external 'WSAStartup@ws2_32.dll stdcall';
function WSACleanup(): Integer;
external 'WSACleanup@ws2_32.dll stdcall';
function WSAGetLastError(): Integer;
external 'WSAGetLastError@ws2_32.dll stdcall';
function socket(af, typ, protocol: Integer): Integer;
external 'socket@ws2_32.dll stdcall';
function bind(s: Integer; name: AnsiString; namelen: Integer): Integer;
external 'bind@ws2_32.dll stdcall';
function closesocket(s: Integer): Integer;
external 'closesocket@ws2_32.dll stdcall';

function PortIsInUse(Port: Integer): Boolean;
var
  s: Integer;
  addr: AnsiString;
begin
  Result := False;
  s := socket(2, 1, 6);  // AF_INET, SOCK_STREAM, IPPROTO_TCP
  if s = -1 then
    exit;
  // sockaddr_in: family AF_INET (little-endian 2), port in network order, INADDR_ANY, 8 zero bytes
  addr := Chr(2) + Chr(0) + Chr(Port div 256) + Chr(Port mod 256) + StringOfChar(Chr(0), 12);
  if bind(s, addr, 16) <> 0 then
    Result := (WSAGetLastError() = 10048);  // WSAEADDRINUSE
  closesocket(s);
end;

// Negative when one of the three classic ports is taken, as the DLL answered.
function CheckPorts(): Integer;
var
  wsaData: AnsiString;
begin
  Result := 0;
  wsaData := StringOfChar(Chr(0), 512);
  if WSAStartup($0202, wsaData) <> 0 then
    exit;
  if PortIsInUse(25) or PortIsInUse(110) or PortIsInUse(143) then
    Result := -1;
  WSACleanup();
end;

// get Windows Installer version
procedure DecodeVersion(const Version: cardinal; var a, b : word);
begin
  a := word(Version shr 16);
  b := word(Version and not $ffff0000);
end;


function OpenServiceManager() : HANDLE;
begin
	if UsingWinNT() = true then begin
		Result := OpenSCManager('','ServicesActive',SC_MANAGER_ALL_ACCESS);
		if Result = 0 then
			SuppressibleMsgBox('the servicemanager is not available', mbError, MB_OK, IDOK)
	end
	else begin
			SuppressibleMsgBox('only nt based systems support services', mbError, MB_OK, IDOK)
			Result := 0;
	end
end;

function IsServiceInstalled(ServiceName: AnsiString) : boolean;
var
	hSCM	: HANDLE;
	hService: HANDLE;
begin
	hSCM := OpenServiceManager();
	Result := false;
	if hSCM <> 0 then begin
		hService := OpenService(hSCM,ServiceName,SERVICE_QUERY_CONFIG);
        if hService <> 0 then begin
            Result := true;
            CloseServiceHandle(hService)
		end;
        CloseServiceHandle(hSCM)
	end
end;

function StopService(ServiceName: AnsiString) : boolean;
var
	hSCM	: HANDLE;
	hService: HANDLE;
	Status	: SERVICE_STATUS;
begin
	hSCM := OpenServiceManager();
	Result := false;
	if hSCM <> 0 then begin
		hService := OpenService(hSCM,ServiceName,SERVICE_STOP);
        if hService <> 0 then begin
        	Result := ControlService(hService,SERVICE_CONTROL_STOP,Status);
            CloseServiceHandle(hService)
		end;
        CloseServiceHandle(hSCM)
	end;
end;

function IsServiceRunning(ServiceName: AnsiString) : boolean;
var
	hSCM	: HANDLE;
	hService: HANDLE;
	Status	: SERVICE_STATUS;
begin
	hSCM := OpenServiceManager();
	Result := false;
	if hSCM <> 0 then begin
		hService := OpenService(hSCM,ServiceName,SERVICE_QUERY_STATUS);
    	if hService <> 0 then begin
			if QueryServiceStatus(hService,Status) then begin
				Result :=(Status.dwCurrentState = SERVICE_RUNNING)
        	end;
            CloseServiceHandle(hService)
		    end;
        CloseServiceHandle(hSCM)
	end
end;

function IsServiceStopped(ServiceName: AnsiString) : boolean;
var
	hSCM	: HANDLE;
	hService: HANDLE;
	Status	: SERVICE_STATUS;
begin
	hSCM := OpenServiceManager();
	Result := false;
	if hSCM <> 0 then begin
		hService := OpenService(hSCM,ServiceName,SERVICE_QUERY_STATUS);
    	if hService <> 0 then begin
			if QueryServiceStatus(hService,Status) then begin
				Result :=(Status.dwCurrentState = SERVICE_STOPPED)
        	end;
            CloseServiceHandle(hService)
		    end;
        CloseServiceHandle(hSCM)
	end
end;

function GetInifile() : AnsiString;
var
   szInifile : String;
begin

   // Check if the file exists in the selected installation directory.
   szInifile := ExpandConstant('{app}\Bin\hMailServer.ini');

   if (FileExists(szInifile) = False) then
   begin

      if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1','InstallLocation', szInifile) then
      begin
         szInifile := szInifile + 'Bin\hMailServer.ini';
      end;

      if (FileExists(szInifile) = False) then
      begin
        // File doesn't exist in the old installation directory.
        szInifile := ExpandConstant('{win}\hMailServer.ini');

        if (FileExists(szInifile) = False) then
        begin
           szInifile := '';
        end;
      end;

   end;

   Result := szInifile;

end;

// Whether a password was collected, on the wizard's page, with /adminpassword or in
// HM_ADMIN_PASSWORD.
// WriteAdministratorPassword writes one only then.
//
// Without that rule, an installation that never collects a password - every silent
// install, because the password page only exists in the interactive wizard - wrote
// GetMD5OfString('') into the ini: 'd41d8cd98f00b204e9800998ecf8427e', the MD5 of
// the empty string. That is not "no password set", it is a valid password entry
// that the server happily authenticates an *empty* password against, because
// Crypt::GetHashType treats any 32-character value as an MD5 hash. It also turned
// off the two protections that key off an unset password: RestApiServer::Start
// refuses to start the REST admin API while the administrator password is empty,
// and ShouldSkipPage stops offering the password page once the ini value is
// non-empty - so setup could never be used to correct the machine afterwards.
//
// Leaving the key out is the safer of the two states and it is the one the rest of
// the product is written for. Unattended installs can supply a real password with
// /adminpassword=<password>, or in HM_ADMIN_PASSWORD (see InitializeSetup).
function HasAdministratorPassword(): Boolean;
begin
  Result := Length(g_szAdminPassword) > 0;
end;

// The time the hMailServer service is given to stop, derived from
// ShutdownDrainSeconds rather than guessed: that setting deliberately holds the
// stop open while sessions finish, so a fixed timeout would have been wrong on
// exactly the installations that use it. 30 seconds of headroom on top of the
// configured drain period.
function GetServiceStopBudgetMs() : Integer;
var
   szIniFile   : AnsiString;
   drainSeconds: Integer;
begin
   drainSeconds := 0;
   szIniFile := GetInifile();
   if (Length(szIniFile) > 0) then
      drainSeconds := GetIniInt('Settings', 'ShutdownDrainSeconds', 0, 0, 3600, szIniFile);

   Result := (drainSeconds + 30) * 1000;
end;

// The id of the process the hMailServer service runs in, while it has one: 0 once
// the service has stopped, and when it is not installed or cannot be asked.
function GetHMailServerProcessId() : cardinal;
var
	hSCM	: HANDLE;
	hService: HANDLE;
	Status	: SERVICE_STATUS_PROCESS;
	needed	: cardinal;
begin
	Result := 0;
	hSCM := OpenServiceManager();
	if hSCM <> 0 then begin
		hService := OpenService(hSCM,'hMailServer',SERVICE_QUERY_STATUS);
		if hService <> 0 then begin
			if QueryServiceStatusProcess(hService, SERVICE_STATUS_PROCESS_INFO, Status, SERVICE_STATUS_PROCESS_SIZE, needed) then
				Result := Status.dwProcessId;
			CloseServiceHandle(hService)
		end;
		CloseServiceHandle(hSCM)
	end
end;

// Stops the hMailServer service and waits, with bounds, until its PROCESS has
// exited - not only until the service says it has stopped. Returns False when the
// service did not stop within budgetMs, or its process did not exit within
// PROCESS_EXIT_BUDGET_MS after that; True when both happened, and when there is no
// service to stop. Called before the program files are replaced (StopHMailServerService,
// at wpReady) and before they are deleted (CurUninstallStepChanged, at usUninstall).
//
// The service reports STOPPED from TerminateServer (hMailServer.cpp) once the
// servers are down and the database is closed, and the process then takes about
// 1.1 seconds more to unload and exit: four stops measured 1,091 to 1,128 ms on
// 25 September 2026, while net stop itself returned in 63 to 114 ms. Every file
// the process has loaded - hMailServer.exe, the C++ runtime, OpenSSL, libpq - is in
// use until it has gone, so whatever deletes or replaces them on the strength of
// STOPPED alone runs that race. The uninstall lost it and left seven files in Bin.
// The upgrade ran the same race here, and won it only because Restart Manager's
// scan happened to take 2.5 seconds between the stop and the first file.
//
// The process is opened before the stop is asked for: once the service has
// stopped, the service controller no longer names it, and an id looked up after
// that could already belong to another process. When a service that was not
// stopped has no process this can open - the id could not be read, or the process
// refused the handle - three seconds, about three times the lag measured, stand
// in for the wait, and the log says so.
function StopHMailServerAndWaitForExit(budgetMs: Integer) : Boolean;
var
   processId  : cardinal;
   hProcess   : HANDLE;
   waitedMs   : Integer;
   wasStopped : Boolean;
begin
   Result := true;

   if (IsServiceInstalled('hMailServer') = false) then
      Exit;

   hProcess := 0;
   processId := GetHMailServerProcessId();
   if (processId <> 0) then
      hProcess := OpenProcessForWait(PROCESS_WAIT_ACCESS, 0, processId);

   wasStopped := IsServiceStopped('hMailServer');
   if (wasStopped = false) then
      StopService('hMailServer');

   waitedMs := 0;
   while (IsServiceStopped('hMailServer') = false) and (waitedMs < budgetMs) do
   begin
      Sleep(250);
      waitedMs := waitedMs + 250;
   end;

   if (IsServiceStopped('hMailServer') = false) then
   begin
      Log('The hMailServer service did not stop within ' + IntToStr(budgetMs div 1000) + ' seconds.');
      Result := false;
   end
   else if (hProcess <> 0) then
   begin
      if (WaitForProcessExit(hProcess, PROCESS_EXIT_BUDGET_MS) <> PROCESS_WAIT_SIGNALLED) then
      begin
         Log('The hMailServer service stopped, but its process did not exit within ' + IntToStr(PROCESS_EXIT_BUDGET_MS div 1000) + ' seconds.');
         Result := false;
      end
      else
         Log('The hMailServer service has stopped and its process has exited.');
   end
   else if (wasStopped = false) then
   begin
      Log('The hMailServer service has stopped. Its process could not be found or opened to wait for, so three seconds stand in for the wait.');
      Sleep(3000);
   end;

   if (hProcess <> 0) then
      CloseProcessForWait(hProcess);
end;

// Stops the hMailServer service before any program file is replaced, and waits,
// with bounds, until its process has exited (StopHMailServerAndWaitForExit).
// Returns False when it has not - the caller must not let the installation
// proceed in that case.
//
// This replaces "while not IsServiceStopped do Sleep(250)" with no limit, which
// had two failure modes, and the second one is the reason this returns a value:
//
//   * A service stuck in STOP_PENDING - or a StopService() that failed outright,
//     which the old code did not look at either - froze the wizard forever with no
//     message, no progress and a dead Cancel button.
//   * If the service is still running when the file copy starts - or its process
//     is still exiting, which the service's STOPPED does not rule out - Inno cannot
//     replace Bin\hMailServer.exe and offers Retry/Ignore. Choosing Ignore leaves
//     the OLD server binary in place while DBSetupQuick/DBUpdater take the schema
//     to the new version, and Application::OnDatabaseConnected then refuses to
//     start the server at all ("The database is too new for this version of
//     hMailServer"). Refusing to move past the Ready page is the only point at
//     which that outcome can still be prevented.
function StopHMailServerService() : Boolean;
var
   budgetMs: Integer;
begin
   Result := true;

   if (IsServiceInstalled('hMailServer') = false) then
      Exit;

   budgetMs := GetServiceStopBudgetMs();

   if (StopHMailServerAndWaitForExit(budgetMs) = false) then
   begin
      SuppressibleMsgBox('The hMailServer service did not stop, or its process did not exit, within ' + IntToStr((budgetMs + PROCESS_EXIT_BUDGET_MS) div 1000) + ' seconds.' + #13#10#13#10 +
             'Setup cannot continue: replacing the program files while the server is running would leave the old ' +
             'server binary installed alongside an upgraded database, and hMailServer would then refuse to start at all.' + #13#10#13#10 +
             'Stop the hMailServer service manually (services.msc), make sure no hMailServer.exe process is left in Task Manager, then run this installation again.', mbError, MB_OK, IDOK);
      Result := false;
   end;
end;

function GetCurrentDatabaseType() : AnsiString;
var
   szInifile : AnsiString;
   szDatabaseType : AnsiString;
begin

   // Locate the ini file.
   szInifile := GetInifile();

   // Read the database settings...
   szDatabaseType := GetIniString('Database', 'Type', '', szIniFile);
   Result := Lowercase(szDatabaseType);
end;


function GetAdministratorPassword() : AnsiString;
	var szIniFile : AnsiString;
	var sKey : AnsiString;
begin
	szIniFile := GetInifile();
	
	sKey := GetIniString('Security', 'AdministratorPassword', '', szIniFile);
	
	Result := sKey;
end;

// Writes the administrator password the wizard's page, /adminpassword or
// HM_ADMIN_PASSWORD collected into [Security] AdministratorPassword, as the salted
// PBKDF2 hash the Control Panel writes: hMailServer.exe --set-admin-password makes
// it, the hash being the server's to make. Until 25 September 2026 an [INI] entry
// wrote GetMD5OfString of the password instead - an unsalted MD5 digest, which the
// server accepted and nothing re-hashed (docs/SecretsAtRest.md).
//
// The password reaches the verb in this process's environment, HM_ADMIN_PASSWORD,
// which the process Exec starts inherits, and never on its command line, where
// anybody listing processes could read it: Exec cannot write to a process's
// standard input. The variable is emptied again as soon as the verb has finished,
// so that no process started after it - the database tools included - inherits it.
//
// Only when a password was collected and the file has none, as the [INI] entry's
// createkeyifdoesntexist did: an upgrade never changes the administrator password.
// Called before the database step, which signs in with this password over COM and
// starts the service, which reads the file when it starts. False when the password
// could not be written; the caller then does not hand it to the database tools,
// which would be refused with it, and the message says what to do.
function WriteAdministratorPassword(): Boolean;
var
  ResultCode: Integer;
  bStarted: Boolean;
begin
  Result := true;

  if (HasAdministratorPassword() = false) or (Length(GetAdministratorPassword()) > 0) then
    Exit;

  bStarted := false;
  ResultCode := 0;
  if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', g_szAdminPassword) then
  begin
    SuppressibleMsgBox('The administrator password could not be set: it could not be handed to hMailServer.exe.' + #13#10#13#10 +
           'hMailServer is installed with no administrator password. Set one in the Control Panel (Settings > Security > Administrative access) before the server is used.', mbError, MB_OK, IDOK);
    Result := false;
    Exit;
  end;
  try
    bStarted := Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '--set-admin-password', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  finally
    SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');
  end;

  if (bStarted = false) then
  begin
    SuppressibleMsgBox('The administrator password could not be set: hMailServer.exe did not start.' + #13#10 + SysErrorMessage(ResultCode) + #13#10#13#10 +
           'hMailServer is installed with no administrator password. Set one in the Control Panel (Settings > Security > Administrative access) before the server is used.', mbError, MB_OK, IDOK);
    Result := false;
  end
  else if (ResultCode <> 0) then
  begin
    SuppressibleMsgBox('The administrator password could not be set (hMailServer.exe --set-admin-password returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
           'hMailServer is installed with no administrator password. Set one in the Control Panel (Settings > Security > Administrative access) before the server is used.', mbError, MB_OK, IDOK);
    Result := false;
  end
  // Exit 0 and still no line: an hMailServer.exe that does not know the verb
  // answers 0 as well. What the file holds is the answer, not the exit code.
  else if (Length(GetAdministratorPassword()) = 0) then
  begin
    SuppressibleMsgBox('The administrator password could not be set: hMailServer.exe --set-admin-password finished, but hMailServer.ini holds no administrator password.' + #13#10#13#10 +
           'Set one in the Control Panel (Settings > Security > Administrative access) before the server is used.', mbError, MB_OK, IDOK);
    Result := false;
  end;
end;

// Returns true when the .NET {#DOTNET_MAJOR} Desktop Runtime is not installed, in
// which case the bundled runtime installer is executed. Also the Check function on
// the [Run] entry in section_run.iss.
//
// The shared framework records one directory per installed version:
//   {pf64}\dotnet\shared\Microsoft.WindowsDesktop.App\<version>
// so the version has to be matched *inside* that folder. Two earlier spellings of
// this were both wrong, and the second one had never worked at all:
//
//   * 'Microsoft.WindowsDesktop.App\8.*' was correct in shape but stayed on 8
//     after the tools moved to .NET 10.
//   * the .NET 10 migration meant to make that '...App\10.*', but what landed in
//     the file was '...App' followed by a raw 0x08 byte and '.*' - a "\10" run
//     through something that read it as an octal escape. A glob containing a
//     control character matches nothing, so DotNetDesktopMissing() returned True
//     unconditionally: the bundled runtime installer ran on every install, twice
//     (once from [Run], whose Check never went false, and again at ssPostInstall
//     from InstallDotNetRuntime), and on a machine that already had a newer 10.x
//     the bundle's 1638 exit code produced a bogus "installation failed" error at
//     the end of an otherwise successful install.
//
// Matching on the major version only is deliberate rather than lax: a
// framework-dependent .NET app rolls forward across minor and patch versions but
// never across a major version, so 10.anything satisfies the tools and 11 would
// not.
function DotNetDesktopMissing(): Boolean;
var
  findRec: TFindRec;
begin
  Result := True;

  if not FindFirst(ExpandConstant('{pf64}\dotnet\shared\Microsoft.WindowsDesktop.App\{#DOTNET_MAJOR}.*'), findRec) then
    Exit;

  try
    repeat
      if (findRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        Result := False;
    until (Result = False) or (not FindNext(findRec));
  finally
    FindClose(findRec);
  end;
end;

// Installs the bundled .NET {#DOTNET_MAJOR} Desktop Runtime when it is missing.
// Called from RunPostInstallTasks before the database tools are executed - they
// are .NET {#DOTNET_MAJOR} apps and run from [Code] at ssPostInstall.
//
// This is the second attempt, not the first, whatever this comment used to say.
// Inno processes a [Run] entry without the postinstall flag BEFORE ssPostInstall
// ("Installation process succeeded", then the [Run] entries, then ssPostInstall:
// the smoke test's install log of 25 September 2026 shows the runtime's [Run]
// entry finishing, eleven seconds on, before the database step began). So it is
// normally installed by section_run.iss's entry, and DotNetDesktopMissing() finds
// it here and returns at once. That order is the safe one and is kept: the
// runtime is in place before the database tools run either way, and when the
// [Run] entry's install failed, this tries again and says so, which the [Run]
// entry does not. Making the [Run] entry postinstall instead would turn it into a
// checkbox on the last page of an interactive install.
//
// True when the runtime is there at the end. The database step needs it; an
// installation hMailServer.Updater can put back stops before that step without it
// (g_szRollbackVersion), and any other carries on to the step, which then fails as
// it always did.
function InstallDotNetRuntime(): Boolean;
	var
		ResultCode: Integer;
begin
	Result := true;
	if not DotNetDesktopMissing() then
		Exit;

	if (Exec(ExpandConstant('{tmp}\{#DOTNET_RUNTIME_FILE}'), '/install /quiet /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime could not be installed. The database setup tools will not work until it is installed.' + #13#10 +
		       SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	// Exec returns True whenever the process launched; the actual install result is
	// the exit code. 0 = success, 3010 = success/reboot required, 1638 = a newer
	// build of the same runtime is already there. Rather than enumerate every
	// benign code the bundle can return, re-run the probe: the only question that
	// matters is whether the runtime is present now.
	if (ResultCode <> 0) and (ResultCode <> ERROR_SUCCESS_REBOOT_REQUIRED) and DotNetDesktopMissing() then
	begin
		SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime installation failed with exit code ' + IntToStr(ResultCode) + '.' + #13#10 +
		       'The database setup tools will not work until the runtime is installed. ' +
		       'Install it manually from ' + DOTNET_DOWNLOAD_URL + ' and then run ' +
		       'DBSetupQuick.exe from the hMailServer Bin folder.', mbError, MB_OK, IDOK);
		Result := false;
	end;
end;

// Whether this installation makes a new database: asked once, of the settings file
// as it was before the installation touched it - an upgrade has a database type
// there, a new installation has none. A profile is offered to a new server only:
// applied to a server already in service, it could close a listener or turn a
// filter off that somebody depends on, and the Control Deck and the Control Panel
// show that server the diff first.
function IsNewDatabase(): Boolean;
begin
   if not g_bNewDatabaseKnown then
   begin
      g_bNewDatabase := (GetCurrentDatabaseType() = '');
      g_bNewDatabaseKnown := true;
   end;
   Result := g_bNewDatabase;
end;

// The external database, given on the command line or in the environment
// (docs/WindowsOnArm.md, "A new installation without a window"). The names are the
// container's (docs/EnvironmentConfiguration.md), and DBSetup /unattended reads the
// variables (Unattended.cs); each switch is its variable's value on the command
// line, and wins over it, as /adminpassword wins over HM_ADMIN_PASSWORD:
//
//   /dbtype=          HM_DB_TYPE           PostgreSQL, MySQL (or MariaDB) or MSSQL
//   /dbhost=          HM_DB_HOST           the database server; for SQL Server
//                                          HOST, HOST\INSTANCE or HOST,PORT
//   /dbport=          HM_DB_PORT           PostgreSQL's and MySQL's port
//   /dbname=          HM_DB_NAME           the database, hMailServer when not given
//   /dbuser=          HM_DB_USER           none for SQL Server: Windows authentication
//                     HM_DB_PASSWORD       the password: never a switch
//   /dbpasswordfile=  HM_DB_PASSWORD_FILE  a file that holds the password
//   /createdatabase=  HM_CREATE_DATABASE   1 creates the database (the default), 0
//                                          uses one that exists
//   /dbservice=       HM_DB_SERVICE        a database server's Windows service on
//                                          this machine, for hMailServer to start after
//
// There is no switch for the password: a command line is read by anybody listing
// processes while setup runs, and Inno drops every double quote from one. The
// installer never reads the password itself; DBSetup does, from the variable or the
// file, as it inherits this process's environment.
function DatabaseSwitch(Name: String): String;
begin
   Result := Trim(ExpandConstant('{param:' + Name + '|}'));
end;

// PostgreSQL, MySQL or MSSQL for each spelling DBSetup takes, as the container's
// entrypoint takes them; '' for anything else - the built-in database included,
// which /useinternaldbms chooses and DBSetup does not set up.
function NormalizedDatabaseType(szType: String): String;
var
   szLower: String;
begin
   szLower := Lowercase(Trim(szType));
   if (szLower = 'postgresql') or (szLower = 'postgres') or (szLower = 'pgsql') then
      Result := 'PostgreSQL'
   else if (szLower = 'mysql') or (szLower = 'mariadb') then
      Result := 'MySQL'
   else if (szLower = 'mssql') then
      Result := 'MSSQL'
   else
      Result := '';
end;

// Hands one switch on to DBSetup in its variable, when the switch was given. A
// variable this process already holds is left as it is when its switch was not.
function ForwardDatabaseSwitch(Name: String; Variable: String): Boolean;
var
   szValue: String;
begin
   Result := true;
   szValue := DatabaseSwitch(Name);
   if szValue <> '' then
      Result := SetProcessEnvironmentVariable(Variable, szValue);
end;

// Empties every variable of the database step, so that no process this installation
// starts after DBSetup - hMailServer.exe's verbs - inherits them, the password least
// of all. g_bDatabaseGiven and g_szDatabaseType keep what was given.
procedure ClearDatabaseVariables();
begin
   SetProcessEnvironmentVariable('HM_DB_TYPE', '');
   SetProcessEnvironmentVariable('HM_DB_HOST', '');
   SetProcessEnvironmentVariable('HM_DB_PORT', '');
   SetProcessEnvironmentVariable('HM_DB_NAME', '');
   SetProcessEnvironmentVariable('HM_DB_USER', '');
   SetProcessEnvironmentVariable('HM_DB_PASSWORD', '');
   SetProcessEnvironmentVariable('HM_DB_PASSWORD_FILE', '');
   SetProcessEnvironmentVariable('HM_CREATE_DATABASE', '');
   SetProcessEnvironmentVariable('HM_DB_SERVICE', '');
end;

// DBSetup /unattended for the database given: the switches handed on in their
// variables, the administrator password in HM_ADMIN_PASSWORD as for the wizard, and
// its lines - the database it set up, or why it did not; never a password - read
// back into the setup log and, for a failure, into szSaid for the message. It shows
// no window. Run through cmd.exe to capture those lines, as /GrantServiceAccess is,
// and cmd.exe returns DBSetup's own exit code: 0 set up, at this build's schema; 1
// the step failed; 2 the settings were refused and nothing was changed; 3 the
// administrator password was refused. False when it could not be started at all.
function RunUnattendedDBSetup(szCredential: String; var ResultCode: Integer; var szSaid: String): Boolean;
var
   szReport: String;
   szLines: AnsiString;
   bHandedOn: Boolean;
begin
   Result := false;
   ResultCode := 0;
   szSaid := '';
   szReport := ExpandConstant('{tmp}\dbsetup-unattended.txt');

   // A password file given as a switch wins over a password in the environment,
   // which DBSetup would otherwise refuse as the two given at once.
   bHandedOn := ForwardDatabaseSwitch('dbtype', 'HM_DB_TYPE') and
                ForwardDatabaseSwitch('dbhost', 'HM_DB_HOST') and
                ForwardDatabaseSwitch('dbport', 'HM_DB_PORT') and
                ForwardDatabaseSwitch('dbname', 'HM_DB_NAME') and
                ForwardDatabaseSwitch('dbuser', 'HM_DB_USER') and
                ForwardDatabaseSwitch('createdatabase', 'HM_CREATE_DATABASE') and
                ForwardDatabaseSwitch('dbservice', 'HM_DB_SERVICE');
   if bHandedOn and (DatabaseSwitch('dbpasswordfile') <> '') then
      bHandedOn := SetProcessEnvironmentVariable('HM_DB_PASSWORD', '') and
                   ForwardDatabaseSwitch('dbpasswordfile', 'HM_DB_PASSWORD_FILE');
   if bHandedOn then
      bHandedOn := SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', szCredential);

   try
      if bHandedOn then
      begin
         Log('Setting up the ' + g_szDatabaseType + ' database given with ' + g_szDatabaseFrom + ': DBSetup.exe /unattended.');
         Result := Exec(ExpandConstant('{cmd}'), '/s /c ""' + ExpandConstant('{app}\Bin\DBSetup.exe') + '" /unattended > "' + szReport + '" 2>&1"',
                        '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
      end;
   finally
      SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');
      ClearDatabaseVariables();
   end;

   if not bHandedOn then
   begin
      szSaid := 'The database settings could not be handed to DBSetup.exe.';
      Log(szSaid);
      Exit;
   end;

   if not Result then
   begin
      szSaid := 'DBSetup.exe could not be run: ' + SysErrorMessage(ResultCode);
      Log(szSaid);
      Exit;
   end;

   if LoadStringFromFile(szReport, szLines) then
      szSaid := Trim(String(szLines));
   if szSaid = '' then
      szSaid := 'DBSetup.exe wrote nothing to ' + szReport + '.';
   Log('DBSetup.exe /unattended returned ' + IntToStr(ResultCode) + ':' + #13#10 + szSaid);
end;

// The profile chosen: on the page, or with /profile=NAME for a silent installation
// - which is itself the explicit choice, as the page is. 'none' when the page's
// None, or /profile=none, chose the shipped settings, which is recorded so that
// the checklist stops asking; empty when nothing was chosen at all.
function GetChosenProfile(): String;
var
   szProfile: String;
begin
   Result := '';

   if (IsComponentSelected('server') = false) or (IsNewDatabase() = false) then
      Exit;

   if WizardSilent() then
   begin
      szProfile := Lowercase(Trim(ExpandConstant('{param:profile|}')));
      if (szProfile = 'office') or (szProfile = 'gateway') or (szProfile = 'relay') or (szProfile = 'sender') or (szProfile = 'none') then
         Result := szProfile;
      Exit;
   end;

   if g_pageProfile = nil then
      Exit;

   case g_pageProfile.SelectedValueIndex of
      0: Result := 'none';
      1: Result := 'office';
      2: Result := 'gateway';
      3: Result := 'relay';
      4: Result := 'sender';
   end;
end;

// Says that the profile was not applied, why, and where it can be applied from; the
// installation still succeeds with the shipped settings. For none, that the choice
// was not recorded, so the checklist will ask.
procedure SayProfileNotApplied(szProfile: String; szWhy: String);
begin
   if szProfile = 'none' then
      SuppressibleMsgBox('The choice to keep the settings this version ships with was not recorded: ' + szWhy + #13#10#13#10 +
             'hMailServer is installed with those settings all the same; the Control Deck''s checklist will ask for a profile until one is chosen there.', mbInformation, MB_OK, IDOK)
   else
      SuppressibleMsgBox('The ' + szProfile + ' profile was not applied: ' + szWhy + #13#10#13#10 +
             'hMailServer is installed with the settings this version ships with. Apply the profile from the Control Deck''s checklist or the Control Panel''s Setting profiles page, which show what it would change first.', mbInformation, MB_OK, IDOK);
end;

// Runs the verb for the profile chosen, after the database step - DBSetupQuick for
// the built-in database, DBSetup for an external one - and before the installation
// starts the service. The database tools reach the server over COM, which starts
// the service, so it is stopped first: the verb refuses to change a running
// server (exit 4), which would not see the settings until a reinitialise. --if-new
// leaves a database already in service alone (exit 5) - DBSetup can attach one
// that is - because a profile applied there with --confirm could close a listener
// or turn a filter off that somebody depends on, without the diff the Control Deck
// shows first. Every failure is said, and the installation still succeeds.
procedure ApplyChosenProfile();
var
   szProfile: String;
   szWhy: String;
   ResultCode: Integer;
begin
   szProfile := GetChosenProfile();
   if szProfile = '' then
      Exit;

   // Stopped, and its process waited for, as the uninstall and the upgrade do:
   // the verb below opens the database the service's process had open, and the
   // service reports STOPPED about a second before that process has gone.
   if (IsServiceRunning('hMailServer') = true) then
   begin
      if (StopHMailServerAndWaitForExit(90000) = false) then
      begin
         SayProfileNotApplied(szProfile, 'the hMailServer service, which the database step started, did not stop, or its process did not exit, within ' + IntToStr((90000 + PROCESS_EXIT_BUDGET_MS) div 1000) + ' seconds, and a profile is not applied under a running server.');
         Exit;
      end;
   end;

   if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '--apply-profile ' + szProfile + ' --once --if-new --confirm', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
   begin
      SayProfileNotApplied(szProfile, 'hMailServer.exe did not start. ' + SysErrorMessage(ResultCode));
      Exit;
   end;

   if (ResultCode = 0) then
      Exit;

   case ResultCode of
      2: szWhy := 'hMailServer.exe did not find the database, or did not know the profile (exit code 2).';
      4: szWhy := 'the hMailServer service was running again when the profile was to be applied (exit code 4).';
      5: szWhy := 'the database already holds ten accounts or more, so it is in service, and a profile is applied to it only after its diff has been seen (exit code 5).';
   else
      szWhy := 'hMailServer.exe --apply-profile returned ' + IntToStr(ResultCode) + ', and the error log says why.';
   end;
   SayProfileNotApplied(szProfile, szWhy);
end;

// Makes this installation's stored-secrets key (docs/SecretsAtRest.md) on a new
// installation, after the database step has created the database and before the
// installation starts the service: hMailServer.exe --create-stored-secrets-key writes
// it into [Security] StoredSecretsKey under this machine's DPAPI and records it in the
// database. The service never makes a key - it may run as an account that cannot
// write hMailServer.ini - and an upgrade gets none here: the setup checklist and the
// upgrade notes say how to turn sealing on. The service is stopped first if the
// database step started it, so that its first start has the key. Every failure is
// said, and the installation still succeeds with the secrets stored as written, as
// they were before. Once made, the key has to be exported and kept with the database
// backups, which the installer cannot do for anybody: the last message says so.
procedure CreateStoredSecretsKey();
var
   ResultCode: Integer;
begin
   if (IsComponentSelected('server') = false) or (IsNewDatabase() = false) then
      Exit;

   // Stopped, and its process waited for, as the profile step, the uninstall and
   // the upgrade do (StopHMailServerAndWaitForExit, batch36's): the verb below
   // opens the database the service's process had open, and the service reports
   // STOPPED about a second before that process has gone. A service that will not
   // stop leaves the key unmade, said, rather than made beside a running server
   // that would not take it until it is restarted.
   if (IsServiceRunning('hMailServer') = true) then
   begin
      if (StopHMailServerAndWaitForExit(90000) = false) then
      begin
         SuppressibleMsgBox('The stored-secrets key was not made: the hMailServer service, which the database step started, did not stop, or its process did not exit, within ' + IntToStr((90000 + PROCESS_EXIT_BUDGET_MS) div 1000) + ' seconds.' + #13#10#13#10 +
                'hMailServer is installed, and stores each account''s second factor and the secret settings as written until a key is made. From an elevated prompt in the hMailServer Bin folder: hMailServer.exe --create-stored-secrets-key, restart the service, then hMailServer.exe --export-stored-secrets-key FILE.', mbInformation, MB_OK, IDOK);
         Exit;
      end;
   end;

   if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '--create-stored-secrets-key', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
   begin
      SuppressibleMsgBox('The stored-secrets key was not made: hMailServer.exe did not start. ' + SysErrorMessage(ResultCode) + #13#10#13#10 +
             'hMailServer is installed, and stores each account''s second factor and the secret settings as written until a key is made. From an elevated prompt in the hMailServer Bin folder: hMailServer.exe --create-stored-secrets-key, then hMailServer.exe --export-stored-secrets-key FILE.', mbInformation, MB_OK, IDOK);
      Exit;
   end;

   if (ResultCode <> 0) then
   begin
      SuppressibleMsgBox('The stored-secrets key was not made (hMailServer.exe --create-stored-secrets-key returned ' + IntToStr(ResultCode) + '). The database may already record the key of another server that shares it.' + #13#10#13#10 +
             'hMailServer is installed. Run the verb again from an elevated prompt in the hMailServer Bin folder to read why; where another server holds the database''s key, import it here with hMailServer.exe --import-stored-secrets-key FILE.', mbInformation, MB_OK, IDOK);
      Exit;
   end;

   SuppressibleMsgBox('hMailServer made this installation''s stored-secrets key, which seals each account''s second factor and the secret settings in the database. It exists on this machine alone.' + #13#10#13#10 +
          'Export it now and keep the file with your database backups, stored apart from them: a database restored without it opens none of those secrets. From an elevated prompt in the hMailServer Bin folder:' + #13#10#13#10 +
          'hMailServer.exe --export-stored-secrets-key FILE' + #13#10#13#10 +
          'The Control Deck''s and the Control Panel''s setup checklist ask for this until it is done.', mbInformation, MB_OK, IDOK);
end;

// Says that a new installation's defaults were not applied, why, and what that
// leaves; the installation still succeeds. bCanRunAgain when the database still
// carries its mark untouched, so that running the verb by hand would do it: not
// after an exit 5, which removed the mark, nor in general after an exit 1, which
// may have changed part of the server - the verb then leaves it as it is, and the
// checklist's "Passwords only over TLS" item is the way. An exit 1 that changed
// nothing keeps the mark, and its szWhy says so.
procedure SayNewInstallDefaultsNotApplied(szWhy: String; bCanRunAgain: Boolean);
var
   szThen: String;
begin
   if bCanRunAgain then
      szThen := 'The Control Deck''s checklist says what is left; ' +
                'or run "hMailServer.exe --apply-new-install-defaults" from the Bin folder with the service stopped.'
   else
      szThen := 'The Control Deck''s checklist says what is left, and its "Passwords only over TLS" item what to do.';

   SuppressibleMsgBox('The secure defaults of a new installation were not applied: ' + szWhy + #13#10#13#10 +
          'hMailServer is installed, but its listeners on ports 25, 587, 110 and 143 may have no TLS, and a mail client on another machine that signs in may send its password in the clear. ' +
          szThen, mbInformation, MB_OK, IDOK);
end;

// What a new installation starts with (docs/SecureDefaults.md): a self-signed
// certificate made for this machine, STARTTLS offered with it on ports 25, 587,
// 110 and 143, and the Internet IP range requiring TLS for authentication - so
// that no password crosses the network in the clear before anybody has
// configured anything. Run after the database step of a new installation and
// before the profile and the service start, with the service stopped: the verb
// refuses to change a running server (exit 4), which would go on serving its
// listeners without TLS until a restart.
//
// Only to a database this installation CREATED, never one that existed before,
// whatever its size. The installer cannot see whether DBSetup created the
// external database or attached one (its exit code is 0 for both); the database
// can say: COM's CreateInternalDatabase and CreateExternalDatabase mark the
// database they make, SetDefaultDatabase - the attach - does not, and the verb
// changes nothing without the mark (exit 5). So on the built-in branch, where
// DBSetupQuick has just created the database (bBuiltIn), exit 5 means the verb
// and the create step disagree and is said; on the DBSetup branch it is the
// answer for an attached database, and only logged. An upgrade never reaches
// here: IsNewDatabase is false for it, and an upgraded server keeps what it has.
procedure ApplyNewInstallDefaults(bBuiltIn: Boolean);
var
   ResultCode: Integer;
begin
   if (IsComponentSelected('server') = false) or (IsNewDatabase() = false) then
      Exit;

   // Stopped and its process gone, not only reported stopped: the verb opens the
   // database the service had open (StopHMailServerAndWaitForExit).
   if (IsServiceRunning('hMailServer') = true) then
   begin
      if (StopHMailServerAndWaitForExit(90000) = false) then
      begin
         SayNewInstallDefaultsNotApplied('the hMailServer service, which the database step started, did not stop within 90 seconds.', true);
         Exit;
      end;
   end;

   if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '--apply-new-install-defaults', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
   begin
      Log('hMailServer.exe --apply-new-install-defaults did not start: ' + SysErrorMessage(ResultCode));
      SayNewInstallDefaultsNotApplied('hMailServer.exe did not start. ' + SysErrorMessage(ResultCode), true);
      Exit;
   end;

   Log('hMailServer.exe --apply-new-install-defaults exited ' + IntToStr(ResultCode) + '.');

   case ResultCode of
      0: Exit;
      5:
         begin
            if bBuiltIn then
               SayNewInstallDefaultsNotApplied('the built-in database DBSetupQuick has just created was not recognised as a new one (exit code 5), which should not happen; its listeners were left as the create step made them.', false);
         end;
      2: SayNewInstallDefaultsNotApplied('hMailServer.exe did not find the database, could not open it, or could not read it well enough to tell whether it is new (exit code 2).', true);
      4: SayNewInstallDefaultsNotApplied('the hMailServer service was running again when the defaults were to be applied (exit code 4).', true);
   else
      SayNewInstallDefaultsNotApplied('hMailServer.exe --apply-new-install-defaults returned ' + IntToStr(ResultCode) + '. The error log says what was and was not changed: where it says nothing was changed (the certificate could not be made, written or recorded), the database keeps its mark, and running it again from the Bin folder with the service stopped still applies them.', false);
   end;
end;

// The account the hMailServer service is registered to log on as, from its entry
// in the registry, as it was before this installation registered anything; '' when
// there was no service - a new installation. Read once, as IsNewDatabase is.
function GetServiceAccountBefore(): String;
var
   szAccount: String;
begin
   if not g_bServiceAccountKnown then
   begin
      if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Services\hMailServer', 'ObjectName', szAccount) then
         szAccount := '';
      g_szServiceAccountBefore := Trim(szAccount);
      g_bServiceAccountKnown := true;
   end;
   Result := g_szServiceAccountBefore;
end;

// Whether this installation creates the service rather than finding one.
function IsNewService(): Boolean;
begin
   Result := (GetServiceAccountBefore() = '');
end;

function IsLocalSystemAccount(Account: String): Boolean;
begin
   Result := (CompareText(Account, 'LocalSystem') = 0) or (CompareText(Account, '.\LocalSystem') = 0) or
             (CompareText(Account, 'NT AUTHORITY\SYSTEM') = 0);
end;

// The account chosen for the service: virtual (NT SERVICE\hMailServer),
// localsystem or keep. On the page, or with /serviceaccount= for a silent
// installation, which is itself the choice; otherwise what the page offers first:
// the virtual account for a new installation, the account the service has for an
// upgrade. Anything else given with /serviceaccount= is ignored, and said in the
// setup log.
function GetServiceAccountChoice(): String;
var
   szChoice: String;
begin
   if IsNewService() then
      Result := 'virtual'
   else
      Result := 'keep';

   szChoice := Lowercase(Trim(ExpandConstant('{param:serviceaccount|}')));
   if (szChoice = 'virtual') or (szChoice = 'localsystem') or (szChoice = 'keep') then
   begin
      Result := szChoice;
      Exit;
   end;
   if szChoice <> '' then
      Log('/serviceaccount=' + szChoice + ' is none of virtual, localsystem or keep, and is ignored.');

   if WizardSilent() or (g_pageServiceAccount = nil) then
      Exit;

   if (g_pageServiceAccount.SelectedValueIndex = 1) then
   begin
      if IsNewService() then
         Result := 'localsystem'
      else if IsLocalSystemAccount(GetServiceAccountBefore()) then
         Result := 'virtual'
      else
         Result := 'localsystem';
   end;
end;

// Whether the service runs as something other than LocalSystem once it is
// registered with the choice - which is when its folders have to be granted to it.
// An upgrade that keeps LocalSystem is not granted anything: nothing would use the
// grants, and the first grant of a large message store takes minutes.
function ServiceNeedsGrants(Choice: String): Boolean;
begin
   if Choice = 'virtual' then
      Result := true
   else if Choice = 'localsystem' then
      Result := false
   else
      Result := (GetServiceAccountBefore() <> '') and (IsLocalSystemAccount(GetServiceAccountBefore()) = false);
end;

// Whether the service is moved to the virtual account AFTER the database step:
// a new installation, or an upgrade of a LocalSystem service, that chose it. The
// database step writes [Database] into hMailServer.INI through the service -
// DBSetup and DBSetupQuick reach it over COM - and sets the dependency on a
// local database server, and the virtual account may do neither: it is given no
// write to the file and no right to change its own service entry
// (docs/ServiceAccount.md). So the service is registered as LocalSystem, or kept
// on it, for that step, and moved once it is done - by then the database names
// every key file, and the grant can list those outside its folders before the
// move is made.
function MoveServiceAfterDatabase(Choice: String): Boolean;
begin
   Result := (Choice = 'virtual') and (IsNewService() or IsLocalSystemAccount(GetServiceAccountBefore()));
end;

// The account the service is registered with before the database step.
function FirstRegistrationChoice(Choice: String): String;
begin
   if MoveServiceAfterDatabase(Choice) then
   begin
      if IsNewService() then
         Result := 'localsystem'
      else
         Result := 'keep';
   end
   else
      Result := Choice;
end;

// The sentence the move page adds when the database is SQL Server reached with
// Windows authentication: the moved service signs in to it as
// NT SERVICE\hMailServer, which has no login there until one is made.
function MssqlIntegratedSentence(): String;
var
   szIniFile: String;
begin
   Result := '';
   szIniFile := GetInifile();
   if szIniFile = '' then
      Exit;
   if (CompareText(GetIniString('Database', 'Type', '', szIniFile), 'MSSQL') = 0) and
      (Trim(GetIniString('Database', 'Username', '', szIniFile)) = '') then
      Result := ' The database is SQL Server with Windows authentication: before moving, give the account a login there - CREATE LOGIN [NT SERVICE\hMailServer] FROM WINDOWS, and a user in the hMailServer database - or the service cannot open it.';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
   Result := false;

   if (WizardSilent() = false) then
   begin
	   // Check if we should skip the password dialog.
	   if (PageID = g_pageAccessKey.ID) then
	   begin
	
	      // If server installation has not been selected, we should skip it,
	      // since the password is for the server...
	      if (IsComponentSelected('server') = false) then
	      begin
	         Result := true;
	      end;
	
	      // If a password has already been specified, we should skip it as well,
	      // It's not possible to change an existing password.
	   	if Length(GetAdministratorPassword()) > 0 then
	   	begin
		   	// Password already specified - skip page.
		   	  Result:= true;
		   end;

		   // ...and likewise when it came in on the command line or in the
		   // environment, so that /adminpassword and HM_ADMIN_PASSWORD work for an
		   // attended install too.
		   if HasAdministratorPassword() then
		      Result := true;
	   end;
	
     // The profile page: a new installation of the server only.
     if (PageID = g_pageProfile.ID) then
     begin
       if (IsComponentSelected('server') = false) or (IsNewDatabase() = false) then
          Result := true;
     end;

     // The service account page: whenever the server is installed, new or upgraded.
     if (PageID = g_pageServiceAccount.ID) then
     begin
       if (IsComponentSelected('server') = false) then
          Result := true;
     end;

     if (PageID = g_pageDBType.ID) then
     begin
       if (GetCurrentDatabaseType() <> '') then
       begin
          // we have already selected database engine. don't ask for it again.
          Result := true;
       end;

       if (IsComponentSelected('server') = false) then
       begin
          Result := true;
       end;

       // ...nor when the command line or the environment gave it.
       if g_bDatabaseGiven then
          Result := true;
     end;

  end;

end;


procedure rdoUseInternal_Click(Sender: TObject);
begin
   g_bUseInternal := true;
end;

procedure rdoUseExternal_Click(Sender: TObject);
begin
   g_bUseInternal := false;
end;

procedure moreInfoLink_Click(Sender: TObject);
var
  ErrorCode: Integer;
begin
  ShellExec('', 'https://www.progressiverobot.com/documentation/?page=choosing_database_engine', '', '', SW_SHOW, ewNoWait, ErrorCode);

end;

procedure CreateWizardPages();
var
   moreInfoLink : TNewStaticText;
   intro : TNewStaticText;
   szAccount : String;

begin
   g_pageDBType := CreateCustomPage(wpSelectComponents, 'Select database server type', 'Choose where hMailServer stores its data');

   intro := TNewStaticText.Create(g_pageDBType);
   with intro do
   begin
     Parent := g_pageDBType.Surface;
     AutoSize := False;
     WordWrap := True;
     Left := ScaleX(0);
     Top := ScaleY(6);
     Width := g_pageDBType.SurfaceWidth;
     Height := ScaleY(30);
     Caption := 'hMailServer can use its own built-in database, or connect to an external database server you already run.';
   end;

    { built-in database }
   rdoUseInternal := TRadioButton.Create(g_pageDBType);
   with rdoUseInternal do
   begin
     Parent := g_pageDBType.Surface;
     Left := ScaleX(8);
     Top := ScaleY(48);
     Width := g_pageDBType.SurfaceWidth - ScaleX(16);
     Height := ScaleY(20);
     Caption := 'Use the built-in database engine (no separate database server required - recommended)';
     TabOrder := 0;
     Checked := g_bUseInternal;
     OnClick := @rdoUseInternal_Click;
#ifdef HM_ARM64
     // SQL Server Compact has no ARM64 build (docs/WindowsOnArm.md).
     Caption := 'The built-in database engine (SQL Server Compact) is not available on ARM64';
     Enabled := False;
     Checked := False;
#endif
   end;

   { external database }
   rdoUseExternal := TRadioButton.Create(g_pageDBType);
   with rdoUseExternal do
   begin
     Parent := g_pageDBType.Surface;
     Left := ScaleX(8);
     Top := ScaleY(84);
     Width := g_pageDBType.SurfaceWidth - ScaleX(16);
     Height := ScaleY(20);
     Caption := 'Use an external database engine (Microsoft SQL Server, MySQL/MariaDB or PostgreSQL)';
     TabOrder := 1;
     Checked := not g_bUseInternal;
     OnClick := @rdoUseExternal_Click;
#ifdef HM_ARM64
     // MySQL and MariaDB need an ARM64 libmysql.dll that this installer does not carry.
     Caption := 'Use an external database engine (Microsoft SQL Server or PostgreSQL; MySQL/MariaDB needs an ARM64 client)';
     Checked := True;
#endif
   end;

   moreInfoLink := TNewStaticText.Create(g_pageDBType);
   with moreInfoLink do
   begin
     Parent := g_pageDBType.Surface;
     Left := ScaleX(8);
     Top := ScaleY(120);
     Width := ScaleX(360);
     Height := ScaleY(20);
     Cursor := crHand;
     Font.Style := [fsUnderline];
     Font.Color := clBlue;
     Caption := 'More information about choosing a database engine...';
     OnClick := @moreInfoLink_Click;
   end;

 	 // Create key page
   g_pageAccessKey := CreateInputQueryPage(wpSelectTasks, 'hMailServer Security', 'Specify main password','The installation program will now create a hMailServer user with administration rights. Please enter a password below. You will need this password to be able to manage your hMailServer installation, so please remember it.');	

   g_pageAccessKey.Add('Password:', True);
   g_pageAccessKey.Add('Confirm password:', True);

   // The last question before the installation: the kind of server this is. One
   // sentence each, naming the listener a profile closes and the filter or the trust
   // it changes, because choosing one here is the confirmation the server asks for.
   g_pageProfile := CreateInputOptionPage(g_pageAccessKey.ID, 'Server profile', 'What kind of server is this?',
      'A profile sets about thirty settings for one kind of server, each with its reason, and keeps what it replaced so that it can be put back. ' +
      'Every profile and its changes can be seen, applied or put back later from the Control Deck and the Control Panel.', True, False);
   g_pageProfile.Add('None - keep the settings this version ships with');
   g_pageProfile.Add('A small office server - spam filtering, greylisting and the auto-ban on, spam held in the quarantine, passwords held to a policy');
   g_pageProfile.Add('Behind a filtering gateway - the address tests judge the real sender once the gateway is an Incoming relay, greylisting is off, and XCLIENT and ARC are trusted from the addresses and the sealer you will name');
   g_pageProfile.Add('An outbound relay - closes IMAP and POP3, suppresses addresses that bounce, and keeps three months of delivery trace');
   g_pageProfile.Add('A high-volume sender - the relay, with more deliveries at once, per-destination ceilings and the feedback loops read; closes IMAP and POP3');
   g_pageProfile.SelectedValueIndex := 0;

   // The account the service runs as (docs/ServiceAccount.md). The page says which
   // applies - the virtual account for a new installation, the service's own account
   // for an upgrade - and the second choice is always the way to the other one,
   // LocalSystem included. Setup grants the account its folders before the service
   // is started (hMailServer.exe /GrantServiceAccess).
   szAccount := GetServiceAccountBefore();
   if IsNewService() then
   begin
      g_pageServiceAccount := CreateInputOptionPage(g_pageProfile.ID, 'Service account', 'Which account should the hMailServer service run as?',
         'hMailServer reads mail from anybody on the internet, so the less its account may do, the less a flaw in it is worth. ' +
         'NT SERVICE\hMailServer is a virtual account of its own: no password, and it reaches only the folders setup grants it. ' +
         'LocalSystem, which earlier versions used, is the most privileged account on this computer. The Control Panel can move the service later.', True, False);
      g_pageServiceAccount.Add('NT SERVICE\hMailServer - a virtual account of its own (recommended)');
      g_pageServiceAccount.Add('LocalSystem - the most privileged account, as earlier versions ran it');
   end
   else if IsLocalSystemAccount(szAccount) then
   begin
      g_pageServiceAccount := CreateInputOptionPage(g_pageProfile.ID, 'Service account', 'The hMailServer service runs as LocalSystem',
         'An upgrade keeps the account the service runs as: here LocalSystem, the most privileged account on this computer. ' +
         'NT SERVICE\hMailServer, a virtual account of its own, reaches only the folders setup grants it; a live update is then installed by an administrator, and the auto-ban cannot write firewall rules. ' +
         'Moving it grants the folders once the database is upgraded, and asks first about any folder or key file the configuration names outside them; running setup again, or the Control Panel, moves it back.', True, False);
      g_pageServiceAccount.Add('Keep LocalSystem');
      g_pageServiceAccount.Add('Move it to NT SERVICE\hMailServer, and grant that account its folders');
   end
   else
   begin
      g_pageServiceAccount := CreateInputOptionPage(g_pageProfile.ID, 'Service account', 'The hMailServer service runs as ' + szAccount,
         'An upgrade keeps the account the service runs as, and grants it its folders again for anything this version added. ' +
         'To go back to LocalSystem, the most privileged account on this computer, choose it below; running setup again moves it back.', True, False);
      g_pageServiceAccount.Add('Keep ' + szAccount);
      g_pageServiceAccount.Add('Move it back to LocalSystem');
   end;
   g_pageServiceAccount.SelectedValueIndex := 0;

end;


procedure OverrideInstallationFolder();
var
  installFolder : String;
  
begin
    // If hMailServer has been previously installed, and there's a reg key
    // specifying where it was installed, we should install into the same
    // folder as before. Normally InnoSetup keeps track of this automatically,
    // but not when switching between x86 and x64 installs.
    if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1','InstallLocation', installFolder) then
    begin
       if (Length(installFolder) > 2) then
          WizardForm.DirEdit.Text := installFolder;
    end;
    
end;

procedure InitializeWizard();
begin

   if ExpandConstant('{param:useinternaldbms|true}') = 'true' then
      g_bUseInternal := true
   else
      g_bUseInternal := false;
   // A database given with /dbtype or HM_DB_TYPE is an external one, set up by
   // DBSetup /unattended: it is the choice the database page would have asked for.
   if g_bDatabaseGiven then
      g_bUseInternal := false;
#ifdef HM_ARM64
   // No built-in database on ARM64, whatever the command line says: a new
   // installation is set up against an external database by DBSetup.
   g_bUseInternal := false;
#endif

   OverrideInstallationFolder();

   if (WizardSilent() = false) then
   begin
      CreateWizardPages();
   end;

end;


function InitializeSetup(): Boolean;
	var
		sMessage : AnsiString;
		SoftwareVersion: AnsiString;
begin
	Result := true;

	// The .NET tools require the .NET {#DOTNET_MAJOR} Desktop Runtime; it is
	// bundled and installed automatically when missing, so no up-front framework
	// check is needed (the old .NET Framework 4.5 gate died with the last
	// .NET Framework tools).

	// The wizard's password page does not exist in a silent install, so accept the
	// administrator password on the command line as well. Read here rather than in
	// InitializeWizard because this is the only hook that can refuse to start, and
	// a password too short to be accepted interactively must not be accepted just
	// because it arrived on the command line. When none is given the key is left
	// out of the ini entirely - see HasAdministratorPassword.
	g_szAdminPassword := ExpandConstant('{param:adminpassword|}');
	// The live update's helper passes the single-use token the server issued for the
	// database upgrade; it is forwarded to the database tool as its credential and
	// is not a password: it is never written to the ini.
	g_szUpgradeToken := ExpandConstant('{param:upgradetoken|}');

	// From 6.3.6 the token comes in the environment instead, HM_UPGRADE_TOKEN, when
	// hMailServer.Updater runs this installer: an elevated process's environment is
	// not readable by the administrator's own unelevated programs, and its command
	// line is; and Inno Setup writes the command line into this installer's log.
	// Held to the token's form - 64
	// lowercase hexadecimal characters - or ignored, and taken out of this
	// process's environment as soon as it is read, so that no program this
	// installer starts inherits it.
	g_bAdministratorUpdate := False;
	if g_szUpgradeToken = '' then
	begin
		g_szUpgradeToken := GetEnv('HM_UPGRADE_TOKEN');
		SetProcessEnvironmentVariable('HM_UPGRADE_TOKEN', '');
		if g_szUpgradeToken <> '' then
		begin
			if IsUpgradeTokenForm(g_szUpgradeToken) then
			begin
				g_bAdministratorUpdate := True;
				Log('The database upgrade''s token was given in HM_UPGRADE_TOKEN, by hMailServer.Updater.');
			end
			else
			begin
				Log('HM_UPGRADE_TOKEN is set but is not a token (64 lowercase hexadecimal characters), so it is ignored.');
				g_szUpgradeToken := '';
			end;
		end;
	end;

	// The version hMailServer.Updater puts back if this installation does not
	// complete (g_szRollbackVersion): taken out of the environment at once, as the
	// token is, and believed only beside the Updater's token.
	g_iDatabaseVersionBefore := -1;
	g_iDatabaseVersionAfter := -1;
	g_bDatabaseChanged := False;
	g_szRollbackVersion := GetEnv('HM_UPDATE_ROLLBACK_VERSION');
	SetProcessEnvironmentVariable('HM_UPDATE_ROLLBACK_VERSION', '');
	if g_szRollbackVersion <> '' then
	begin
		if g_bAdministratorUpdate and IsReleaseVersionForm(g_szRollbackVersion) then
			Log('hMailServer.Updater holds hMailServer ' + g_szRollbackVersion + ' to put back (HM_UPDATE_ROLLBACK_VERSION): a step before the database step that does not complete stops this installation before the database step.')
		else
		begin
			Log('HM_UPDATE_ROLLBACK_VERSION is set, but not beside hMailServer.Updater''s token or not as a version, so it is ignored.');
			g_szRollbackVersion := '';
		end;
	end;

	// Or in the environment, HM_ADMIN_PASSWORD, when neither switch is given - the
	// name the server's own --set-admin-password reads. Inno Setup's command line
	// cannot carry every password: a double quote in it only turns quoting on or
	// off and is dropped, so /adminpassword=ab"cd sets abcd, and the switch is on
	// the command line for anybody listing processes while setup runs. Not read
	// beside /upgradetoken, so that a variable left in the service's environment
	// cannot stand in for the live update's token.
	if (g_szAdminPassword = '') and (g_szUpgradeToken = '') then
		g_szAdminPassword := GetEnv('HM_ADMIN_PASSWORD');

	if (Length(g_szAdminPassword) > 0) and (Length(g_szAdminPassword) < 5) then
	begin
		SuppressibleMsgBox('The password given with /adminpassword or HM_ADMIN_PASSWORD must be at least 5 characters long.', mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	// The external database a new installation is set up against without a window
	// (DatabaseSwitch): read once, here, before anything this installation runs
	// empties the variables. It is held to what DBSetup takes in PrepareToInstall,
	// once the settings file says whether this is a new installation: an upgrade
	// keeps its database, and is not refused for a variable it does not use.
	g_szDatabaseType := DatabaseSwitch('dbtype');
	g_szDatabaseFrom := '/dbtype';
	if g_szDatabaseType = '' then
	begin
		g_szDatabaseType := Trim(GetEnv('HM_DB_TYPE'));
		g_szDatabaseFrom := 'HM_DB_TYPE in the environment';
	end;
	g_bDatabaseGiven := (g_szDatabaseType <> '');
	if NormalizedDatabaseType(g_szDatabaseType) <> '' then
		g_szDatabaseType := NormalizedDatabaseType(g_szDatabaseType);

	// The two checks by window title are for an installation somebody started by
	// hand. An administrator's install (g_bAdministratorUpdate) comes from
	// hMailServer.Updater, which has ended every program running from the
	// installation before it ran this, the Control Panel that asked included; a
	// window of the same title anywhere else - a stray copy, or any program that
	// names a window so - would otherwise stop the install it asked for.
	if (not g_bAdministratorUpdate) and (FindWindowByWindowName('hMailServer Control Panel') > 0) then
	begin
		SuppressibleMsgBox('hMailServer Control Panel is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
		Result := false;
		Exit;
	end;

	// Guard against a legacy hMailServer Administrator from an earlier version
	// still running (it is no longer shipped, but may linger on upgrades).
	if (not g_bAdministratorUpdate) and (FindWindowByWindowName('hMailServer Administrator') > 0) then
	begin
		SuppressibleMsgBox('hMailServer Administrator is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
		Result := false;
		Exit;
	end;
	
	if (Result = true) then
	begin
		if (FindWindowByWindowName('hMailServer Database Setup') > 0) then
		begin
			SuppressibleMsgBox('hMailServer Database Setup is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
			Result := false;
			Exit;
		end;	
	end;
	
	if (Result = true) then
	begin
		if (FindWindowByWindowName('hMailServer Database Upgrader') > 0) then
		begin
			SuppressibleMsgBox('hMailServer Database Upgrader is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
			Result := false;
			Exit;			
		end;	
	end;	
	
	if (Result = true) then
	begin
		if (FindWindowByWindowName('DBSetup') > 0) then
		begin
			SuppressibleMsgBox('hMailServer DBSetup is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
			Result := false;
			Exit;			
		end;	
	end;	
	

	// Check so that there isn't already a server running
	// on one of hour ports.
	if (Result = true) then
	begin
		if (IsServiceRunning('hMailServer') <> True) And (CheckPorts() < 0) then
		begin
			// The hMailServer isn't running, but someone is blocking the ports.
			//
			sMessage := 'The hMailServer Setup has detected that one or several of the TCP/IP ports 25, 110 and 143 are already in use.' + Chr(13) + Chr(10) + 'This indicates that there already is an email server running on this computer.' + Chr(13) + Chr(10) + 'If you plan to use any of these ports with hMailServer, the already existing server must be stopped.';
			SuppressibleMsgBox(sMessage, mbInformation, MB_OK, IDOK);	
		end;
	end;	

	Result := true;

end;



function RegisterTypeLib() : Boolean;
var
  ResultCode: Integer;
begin
   Result := true;

   // Registers the COM type library and the AppID. Exec's own return value only
   // says whether the process started; hMailServer.exe returns -1 when
   // RegisterAppId or RegisterServer fails (see hMailServer.cpp), and that used to
   // be discarded - producing a "Remote administration support" installation whose
   // COM API silently did not work, with a successful-looking wizard.
   if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '/RegisterTypeLib', '',  SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
   begin
      SuppressibleMsgBox('The hMailServer COM API could not be registered.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
      Result := false;
   end
   else if (ResultCode <> 0) then
   begin
      SuppressibleMsgBox('The hMailServer COM API could not be registered (hMailServer.exe /RegisterTypeLib returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
             'Scripts and remote administration tools will not be able to connect until it is. ' +
             'Run "hMailServer.exe /RegisterTypeLib" as an administrator from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK);
      Result := false;
   end;
end;

// Sets the service failure actions. hMailServer.exe /Register creates the service
// with auto-start and a dependency on RPCSS (ServiceManager::RegisterService) but
// never calls ChangeServiceConfig2, so the recovery configuration was Windows'
// default of "take no action" - a mail server that died stayed dead until somebody
// noticed. The installer is the natural place to set this: it is elevated and the
// service has just been created.
//
// reset= 86400 means a day without a failure clears the counter, so the escalating
// delays apply per incident instead of accumulating over the life of the machine.
// Three restarts rather than an unbounded restart loop: a server that fails four
// times inside a day has a problem that restarting will not fix, and by then the
// SCM has recorded four failures for whoever looks.
procedure ConfigureServiceRecovery();
	var
		ResultCode: Integer;
begin
	// Deliberately not reported to the user. Recovery actions are an improvement on
	// top of a service that is otherwise correctly installed, and an error dialog at
	// the end of a successful installation would do more harm than the missing
	// setting. It goes in the setup log instead.
	if (Exec(ExpandConstant('{sys}\sc.exe'),
	         'failure hMailServer reset= 86400 actions= restart/60000/restart/120000/restart/300000',
	         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
		Log('Could not run sc.exe to set the hMailServer service recovery actions: ' + SysErrorMessage(ResultCode))
	else if (ResultCode <> 0) then
		Log('sc.exe failure returned ' + IntToStr(ResultCode) + '; the hMailServer service will not restart itself after a failure.');

	if (Exec(ExpandConstant('{sys}\sc.exe'),
	         'description hMailServer "hMailServer email server - SMTP, POP3 and IMAP."',
	         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
		Log('Could not run sc.exe to set the hMailServer service description: ' + SysErrorMessage(ResultCode))
	else if (ResultCode <> 0) then
		Log('sc.exe description returned ' + IntToStr(ResultCode) + '.');
end;

// Creates (or reconfigures) the hMailServer service. Returns False when the
// service is not there afterwards, in which case there is no point starting it.
function RegisterHMailServerService(szAccountChoice: String) : Boolean;
	var
		ResultCode: Integer;
		szParameters: String;
begin
	Result := true;

	// The account to register with (FirstRegistrationChoice, or virtual for the
	// move): the virtual account, LocalSystem, or the account the service already
	// has. One word each, so that NT SERVICE\hMailServer never has to survive the
	// command line. /Register takes the account from here and nowhere else.
	szParameters := '--service-account ' + szAccountChoice + ' /Register';
	Log('Registering the hMailServer service: hMailServer.exe ' + szParameters);

	// As with the type library: hMailServer.exe returns -1 when CreateService or
	// ChangeServiceConfig fails, and ignoring it meant an installation with no
	// service at all still ended on "Completed" - the following "net START
	// hMailServer" failed too, and its result was ignored as well.
	if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), szParameters, '',  SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	if (ResultCode <> 0) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created (hMailServer.exe /Register returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
		       'Check the hMailServer error log, then run "hMailServer.exe --service-account ' + szAccountChoice + ' /Register" as an administrator from the ' +
		       'hMailServer Bin folder to retry.', mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	ConfigureServiceRecovery();
end;

// Starts the hMailServer service for the database step of an upgrade, with the one
// argument that says so: "sc start hMailServer --for-database-upgrade". The service
// reads it in ServiceMain (hMailServer.cpp), and when that start finds the database
// older than the new binary - the normal state of an upgrade across a schema step,
// until DBUpdater has moved it - it refuses the start as before, and DBUpdater reads
// the refusal from Connect() as it always has, but HM5011 is not reported. The
// line this writes to the setup log is the record of that start.
//
// Until 25 September 2026 the service was started here by DBSetupQuick's COM
// activation, as any start: every silent upgrade across a schema step wrote HM5011
// to the Application event log as a Critical event 2010, which also registered the
// event source that nothing removed. The installer smoke test found both. The COM
// activation that follows now finds the service running and waits for it, as it
// did whenever the service was already up. A start refused here changes nothing
// else: DBSetupQuick's activation then starts the service as it always did, and
// the refusal is reported as before - the log says why.
procedure StartServiceForDatabaseUpgrade();
	var
		ResultCode: Integer;
begin
	if (Exec(ExpandConstant('{sys}\sc.exe'), 'start hMailServer --for-database-upgrade',
	         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
		Log('Could not run sc.exe to start the hMailServer service for the database upgrade: ' + SysErrorMessage(ResultCode))
	else if (ResultCode <> 0) then
		Log('sc.exe start returned ' + IntToStr(ResultCode) + '; the database step starts the hMailServer service itself, and an old database is reported as HM5011.')
	else
		Log('Started the hMailServer service for the database upgrade (--for-database-upgrade).');
end;

// Runs hMailServer.exe /GrantServiceAccess (Common/Util/ServiceAccount.h), which
// grants the service's own identity, NT SERVICE\hMailServer, the program
// folder's own Data, Logs, Temp and Database folders and the folders an
// administrator approved, and Read on the program's own folders - never a folder
// because hMailServer.INI or the database names it, and never hMailServer.INI.
// In an attended installation it runs with --confirm: when the configuration
// names folders nobody approved, or key files outside the grants, the verb
// itself asks in a box of its own - elevated, as this installer is - before it
// approves or grants anything, and No changes nothing. A silent installation
// cannot be asked, and a move then waits for an administrator (exit 4). What the
// verb said goes into the setup log, line for line. False when it could not be
// run at all; ResultCode is its exit code otherwise.
function RunGrantServiceAccess(var ResultCode: Integer) : Boolean;
	var
		szReport: String;
		szSwitches: String;
		szSaid: AnsiString;
begin
	Result := true;
	szReport := ExpandConstant('{tmp}\grant-service-access.txt');
	szSwitches := '';
	if not WizardSilent() then
		szSwitches := '--confirm ';

	if (Exec(ExpandConstant('{cmd}'), '/s /c ""' + ExpandConstant('{app}\Bin\hMailServer.exe') + '" ' + szSwitches + '/GrantServiceAccess > "' + szReport + '" 2>&1"',
	         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		Log('hMailServer.exe /GrantServiceAccess could not be run: ' + SysErrorMessage(ResultCode));
		Result := false;
		Exit;
	end;

	if LoadStringFromFile(szReport, szSaid) then
		Log('hMailServer.exe ' + szSwitches + '/GrantServiceAccess returned ' + IntToStr(ResultCode) + ':' + #13#10 + String(szSaid))
	else
		Log('hMailServer.exe ' + szSwitches + '/GrantServiceAccess returned ' + IntToStr(ResultCode) + ' and wrote nothing to ' + szReport + '.');
end;

// Why a grant run did not end in 0, in a sentence for a message box.
function GrantOutcomeSentence(ResultCode: Integer): String;
begin
	case ResultCode of
		1: Result := 'a grant failed - the report says which, and why';
		2: Result := 'hMailServer.exe is not in the Bin folder of this installation';
		3: Result := 'it was not run as an administrator';
		4: Result := 'the configuration names folders no administrator has approved for the service, or key and certificate files outside the folders it is granted, and a silent installation does not approve them';
		5: Result := 'the configuration names a folder that is never granted to the service - one in the program folder beside its own Data, Logs, Temp and Database folders, or in Windows, Program Files or a user profile - so the configuration has to change first';
		6: Result := 'you answered No';
	else
		Result := 'it returned ' + IntToStr(ResultCode);
	end;
end;

// For an installation whose service already runs as something other than
// LocalSystem: its folders granted again, for anything this version added,
// before the database step starts it. A failure fails the installation - the
// service may not start - in words of its own (g_bGrantFailed).
function GrantServiceAccess() : Boolean;
	var
		ResultCode: Integer;
begin
	Result := true;

	if (RunGrantServiceAccess(ResultCode) = false) then
	begin
		SuppressibleMsgBox('The hMailServer service account could not be granted its folders: hMailServer.exe could not be run.' + #13#10#13#10 +
		       'Run "hMailServer.exe --confirm /GrantServiceAccess" as an administrator from the hMailServer Bin folder, then start the service.', mbError, MB_OK, IDOK);
		g_bGrantFailed := true;
		Result := false;
		Exit;
	end;

	if (ResultCode = 0) or (ResultCode = 4) then
		Exit;

	SuppressibleMsgBox('The hMailServer service account was not granted all of its folders: ' + GrantOutcomeSentence(ResultCode) + '.' + #13#10#13#10 +
	       'The service may not start until it is. Run "hMailServer.exe --confirm /GrantServiceAccess" as an administrator from the hMailServer Bin folder to see what it says and to retry, ' +
	       'or run this setup again and choose LocalSystem.', mbError, MB_OK, IDOK);
	if (ResultCode <> 6) then
	begin
		g_bGrantFailed := true;
		Result := false;
	end;
end;

// Stops the service the database step started, with a bound, and waits for its
// process to exit (StopHMailServerAndWaitForExit): the account is changed and the
// folders granted only once nothing of the old process holds them. False when it
// is still running.
function StopServiceForMove() : Boolean;
begin
	Result := true;
	if (IsServiceRunning('hMailServer') = false) then
		Exit;

	Result := StopHMailServerAndWaitForExit(90000);
end;

// The move to NT SERVICE\hMailServer, after the database step
// (MoveServiceAfterDatabase): the service stopped, its folders granted - with
// the question the verb asks when the configuration names anything outside them
// - and only then registered as the virtual account. Anything short of a clean
// grant leaves the service on LocalSystem, working, and says why; the
// installation still succeeds.
procedure MoveServiceToVirtualAccount();
	var
		ResultCode: Integer;
		szWhy: String;
begin
	if (StopServiceForMove() = false) then
	begin
		SuppressibleMsgBox('The hMailServer service was not moved to NT SERVICE\hMailServer: it did not stop within 90 seconds, and it is moved only while stopped. It runs as LocalSystem, as before.' + #13#10#13#10 +
		       'Move it later from the Control Panel (Windows service account), or run this setup again.', mbInformation, MB_OK, IDOK);
		Exit;
	end;

	szWhy := '';
	if (RunGrantServiceAccess(ResultCode) = false) then
		szWhy := 'hMailServer.exe could not be run'
	else if (ResultCode <> 0) then
		szWhy := GrantOutcomeSentence(ResultCode);

	if szWhy <> '' then
	begin
		SuppressibleMsgBox('The hMailServer service was not moved to NT SERVICE\hMailServer, and runs as LocalSystem, as before: the grant of its folders did not complete - ' +
		       szWhy + '.' + #13#10#13#10 +
		       'Move it later from the Control Panel (Windows service account), which shows what the grant says, or run this setup again.', mbInformation, MB_OK, IDOK);
		Exit;
	end;

	if (RegisterHMailServerService('virtual') = false) then
	begin
		// RegisterHMailServerService has said why. Registering again with keep
		// changes nothing, and the service is still LocalSystem's.
		Exit;
	end;

	Log('The hMailServer service now runs as NT SERVICE\hMailServer.');
end;


function DeleteOldFiles() : Boolean;
begin

   DeleteFile(ExpandConstant('{app}\Bin\Copyright.txt'));
   DeleteFile(ExpandConstant('{app}\Bin\License.txt'));
   DeleteFile(ExpandConstant('{app}\Bin\Sourcecode.txt'));

   // SURBL's list of two-level domains of 2006, which versions before 6.3.6
   // installed here and read to cut host names for the SURBL lookups and the
   // MX-record test. The server cuts them by the Public Suffix List it compiles
   // in now, and nothing reads the file.
   DeleteFile(ExpandConstant('{app}\Bin\tlds.txt'));

   Result := true;
end;

function InstallSQLCE() : boolean;
var
   ResultCode: Integer;
   szParams: AnsiString;
   szDatabaseType : AnsiString;

   bNewInstallationWithSQLCE : Boolean;
   bUpgradeWithSQLCE : Boolean;
begin
   // Assigned up front: the old version left the function result undefined on the
   // (common) path where no SQL CE install is needed, so the value could not be
   // used and was not - a failed runtime install was reported as success.
   Result := true;

   szDatabaseType := GetCurrentDatabaseType();

   bNewInstallationWithSQLCE := (szDatabaseType = '') and g_bUseInternal;
   bUpgradeWithSQLCE := (szDatabaseType = 'mssqlce');

   // Only install SQL CE if we haven't already choosen another
   // database, or if this is a fresh installation. No point in
   // installing SQL CE if MySQL is used.
   if not (bNewInstallationWithSQLCE or bUpgradeWithSQLCE) then
      Exit;

#ifdef HM_ARM64
   // Unreachable: InitializeWizard never chooses the built-in database on ARM64,
   // and PrepareToInstall refuses an upgrade of one. Said, not attempted, if it is.
   SuppressibleMsgBox('The built-in database (SQL Server Compact) is not available on ARM64.', mbError, MB_OK, IDOK);
   Result := false;
   Exit;
#endif

   // x64 only: ArchitecturesAllowed=x64 in section_setup_64.iss, so IsWin64 is
   // always true here. The x86 branch that used to be here pointed at
   // SSCERuntime_x86-ENU.msi, which section_files_64.iss does not ship into {tmp} -
   // had it ever been reachable it would have failed.
   // The path is quoted because {tmp} is under the user's temp directory, which can
   // contain spaces; unquoted, msiexec parsed it as several arguments.
   szParams := ExpandConstant('/I "{tmp}\SSCERuntime_x64-ENU.msi" /quiet /norestart');

   if (ShellExec('', 'msiexec', szParams, '', SW_SHOW, ewWaitUntilTerminated, ResultCode) = False) then
   begin
      SuppressibleMsgBox('The SQL Server Compact 4.0 runtime could not be installed.' + #13#10 +
             SysErrorMessage(ResultCode) + #13#10#13#10 +
             'hMailServer cannot use its built-in database until it is installed.', mbError, MB_OK, IDOK);
      Result := false;
      Exit;
   end;

   // ShellExec's return value only says that msiexec started; the install result is
   // its exit code, and discarding it meant a failed runtime install was announced
   // as a success and only surfaced later as an unexplained database error.
   // 3010 = installed, reboot required. 1638 = this or a newer version is already
   // installed, which is the normal answer on a repair or a reinstall over an
   // existing mssqlce configuration and must not be treated as a failure.
   if (ResultCode <> 0) and
      (ResultCode <> ERROR_SUCCESS_REBOOT_REQUIRED) and
      (ResultCode <> ERROR_PRODUCT_VERSION) then
   begin
      SuppressibleMsgBox('The SQL Server Compact 4.0 runtime installation failed (msiexec returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
             'hMailServer cannot use its built-in database until it is installed. The installer is ' +
             'SSCERuntime_x64-ENU.msi; install it by hand and then run DBSetupQuick.exe from the ' +
             'hMailServer Bin folder.', mbError, MB_OK, IDOK);
      Result := false;
   end;
end;

// The database's schema version, hm_dbversion, as the hMailServer service reads it:
// COM's Database.CurrentVersion, which needs no sign-in (InterfaceDatabase.cpp) and
// which DBUpdater reads the same way. It goes through the service's own connection,
// and so with the service's account and credentials - which on SQL Server with
// Windows authentication are not this installer's, so a connection of the
// installer's own could be refused where the service's is not. -1 when it cannot be
// read: the service cannot be reached, or has no connection to the database. szWhen
// says when, for the log. Both objects are released before this returns.
function ReadDatabaseVersion(szWhen: String): Integer;
var
   App: Variant;
   Database: Variant;
begin
   Result := -1;
   try
      App := CreateOleObject('hMailServer.Application');
      Database := App.Database;
      Result := Database.CurrentVersion;
      if Result > 0 then
         Log('The database is at version ' + IntToStr(Result) + ' ' + szWhen + ' (Database.CurrentVersion, through the hMailServer service).')
      else
      begin
         Log('The database''s version could not be read ' + szWhen + ': the hMailServer service answered ' + IntToStr(Result) + '.');
         Result := -1;
      end;
   except
      Result := -1;
      Log('The database''s version could not be read ' + szWhen + ': ' + GetExceptionMessage);
   end;
   Database := Unassigned;
   App := Unassigned;
end;

// The version this installation replaces, for its messages.
function PreviousVersionText(): String;
begin
   if g_szRollbackVersion <> '' then
      Result := 'hMailServer ' + g_szRollbackVersion
   else
      Result := 'the earlier version';
end;

// What is known of the database's version on either side of the database step,
// as a clause.
function DatabaseVersionsText(): String;
begin
   if (g_iDatabaseVersionBefore > 0) and (g_iDatabaseVersionAfter > 0) then
      Result := 'it was at version ' + IntToStr(g_iDatabaseVersionBefore) + ' before the step and is at version ' + IntToStr(g_iDatabaseVersionAfter)
   else if (g_iDatabaseVersionBefore > 0) then
      Result := 'it was at version ' + IntToStr(g_iDatabaseVersionBefore) + ' before the step, and its version after the step could not be read'
   else if (g_iDatabaseVersionAfter > 0) then
      Result := 'its version before the step could not be read, and it is at version ' + IntToStr(g_iDatabaseVersionAfter)
   else
      Result := 'its version could not be read before the step or after it';
end;

// The database step on an existing database begins. From here the database may
// have changed (g_bDatabaseChanged) until its version afterwards shows otherwise -
// the service's own start for the step can already move it, with [Database]
// UpgradeOnStart=1 - and the lines this writes are what hMailServer.Updater reads
// in this installation's log beside its exit code (DatabaseStepOf in
// hMailServer.Updater.cpp): an installation that wrote "Database step begun" and
// not "Database step left the database unchanged" is never rolled back, however it
// ended - exit code 11, a crash, the Updater's hour running out, or a service that
// did not stay up after exit code 0.
procedure BeginDatabaseStep();
begin
   g_bDatabaseChanged := true;
   if g_iDatabaseVersionBefore > 0 then
      Log('Database step begun: the database was at version ' + IntToStr(g_iDatabaseVersionBefore) + ' before it.')
   else
      Log('Database step begun: the database''s version before it could not be read, so nothing after the step can show it unchanged.');
end;

// The database step has ended, however it ended: the database's version now, and
// whether it is as it was. A service that is not running is started for the read
// as the step starts it, with --for-database-upgrade (StartServiceForDatabaseUpgrade),
// so that a database the step left older than this build is not reported as
// HM5011 for it.
//
// Where the version cannot be read afterwards - this build's service does not
// start, or its COM server does not answer, which is when the earlier version is
// needed back most - a database that was already at the version this build needs
// (HM_REQUIRED_DB_VERSION), or past it, is still known unchanged: nothing in the
// step moves such a database, so a release that changes no schema is put back as
// any other failed step is. Otherwise a version that cannot be read leaves the
// database "may have changed", and nothing is put back.
procedure EndDatabaseStep();
begin
   if (IsServiceRunning('hMailServer') = false) then
   begin
      Log('The hMailServer service is not running after the database step; it is started to read the database''s version.');
      StartServiceForDatabaseUpgrade();
   end;
   g_iDatabaseVersionAfter := ReadDatabaseVersion('after the database step');
   if (g_iDatabaseVersionBefore > 0) and (g_iDatabaseVersionAfter = g_iDatabaseVersionBefore) then
   begin
      g_bDatabaseChanged := false;
      Log('Database step left the database unchanged, at version ' + IntToStr(g_iDatabaseVersionAfter) + '.');
   end
   else if (g_iDatabaseVersionBefore >= {#HM_REQUIRED_DB_VERSION}) and (g_iDatabaseVersionAfter <= 0) then
   begin
      g_bDatabaseChanged := false;
      Log('Database step left the database unchanged, at version ' + IntToStr(g_iDatabaseVersionBefore) + ': this build needs version {#HM_REQUIRED_DB_VERSION}, ' +
          'which the database had reached before the step, and nothing in the step moves a database at or past it, though its version after the step could not be read.');
   end
   else if (g_iDatabaseVersionBefore > 0) and (g_iDatabaseVersionAfter > 0) then
      Log('Database step changed the database: ' + DatabaseVersionsText() + '.')
   else
      Log('Database step may have changed the database: ' + DatabaseVersionsText() + '.');
end;

// A database step that did not complete and that changed the database, or may have:
// said, so that the log and the message name it, and written for hMailServer.Updater
// ("Database step stopped part-way"), which then says to restore the backup the
// update took before it.
procedure SayDatabaseStepStoppedPartWay();
var
   szWhat: String;
begin
   if (g_iDatabaseVersionBefore > 0) and (g_iDatabaseVersionAfter > 0) then
      szWhat := 'The hMailServer database upgrade did not complete, and it had changed the database before it stopped: ' + DatabaseVersionsText() + '.'
   else
      szWhat := 'The hMailServer database upgrade did not complete, and this installation cannot show that it left the database unchanged: ' + DatabaseVersionsText() + '.';
   Log('Database step stopped part-way: ' + DatabaseVersionsText() + '.');
   SuppressibleMsgBox(szWhat + #13#10#13#10 +
          PreviousVersionText() + ' is not put back, since it may not run on the database as it is now. Once the cause is put right, run DBSetupQuick.exe from the hMailServer Bin folder to finish the upgrade; ' +
          'or restore the backup taken before the update and reinstall ' + PreviousVersionText() + '.', mbError, MB_OK, IDOK);
end;

procedure AppendStep(var szList: String; szStep: String);
begin
   if szList <> '' then
      szList := szList + ', ';
   szList := szList + szStep;
end;

// The steps before the database step that did not complete, as a list for a
// sentence; empty when all did.
function StepsNotCompleteBeforeDatabase(bRuntimeReady, bDatabaseBackendReady, bServiceRegistered, bPasswordWritten: Boolean): String;
var
   szList: String;
begin
   szList := '';
   if not bRuntimeReady then
      AppendStep(szList, 'the .NET {#DOTNET_MAJOR} Desktop Runtime');
   if not bDatabaseBackendReady then
      AppendStep(szList, 'the SQL Server Compact runtime');
   if not bServiceRegistered then
      AppendStep(szList, 'the service''s registration');
   if g_bGrantFailed then
      AppendStep(szList, 'the grant of the service account''s folders');
   if not bPasswordWritten then
      AppendStep(szList, 'the administrator password');
   Result := szList;
end;


function RunPostInstallTasks() : Boolean;
   var
      ResultCode: Integer;
      ProgressPage : TOutputProgressWizardPage;
      szParameters: AnsiString;
      szCredential: String;
      szDBSetupSaid: String;
      szNotComplete: String;
      bRuntimeReady: Boolean;
      bDatabaseBackendReady: Boolean;
      bServiceRegistered: Boolean;
      bPasswordWritten: Boolean;
      bStarted: Boolean;
      bStopBeforeDatabase: Boolean;
      bDatabaseStepOnExisting: Boolean;
begin
   Result := true;
   bRuntimeReady := true;
   bStopBeforeDatabase := false;
   bDatabaseStepOnExisting := false;

   // A database given for a new installation, to one that has a database already:
   // an upgrade keeps the database it has, which the setup log says.
   if g_bDatabaseGiven and (IsNewDatabase() = false) then
      Log('A ' + g_szDatabaseType + ' database was given with ' + g_szDatabaseFrom + '; this installation has a database already (' + GetCurrentDatabaseType() + '), and keeps it.');

   // Created outside the try so that the finally below cannot call Hide() on a nil
   // page if the page itself could not be created.
   ProgressPage := CreateOutputProgressPage('Finalizing installation','Please wait while the setup performs post-installation tasks');

   try
      try
         ProgressPage.Show();

         ProgressPage.SetText('Installing the .NET {#DOTNET_MAJOR} Desktop Runtime...', '');
         ProgressPage.SetProgress(1,6);

         // The database tools below are .NET {#DOTNET_MAJOR} apps; make sure the
         // runtime is in place before they are executed. section_run.iss's [Run]
         // entry has normally installed it already - Inno runs it before
         // ssPostInstall - and this is the second attempt when it did not.
         bRuntimeReady := InstallDotNetRuntime();

         ProgressPage.SetText('Initializing database backend...', '');
         ProgressPage.SetProgress(2,6);

         bDatabaseBackendReady := InstallSQLCE();

         ProgressPage.SetText('Creating the hMailServer service...', '');
         ProgressPage.SetProgress(3,6);

         // The account for the database step (FirstRegistrationChoice): a
         // service to be moved to the virtual account is LocalSystem's until the
         // database is made or upgraded, and moved after it.
         bServiceRegistered := RegisterHMailServerService(FirstRegistrationChoice(GetServiceAccountChoice()));

         // Before anything starts the service - the database step below does, over
         // COM - a service that already runs as something other than LocalSystem
         // is granted its folders again (ServiceNeedsGrants).
         if bServiceRegistered and ServiceNeedsGrants(GetServiceAccountChoice()) and (MoveServiceAfterDatabase(GetServiceAccountChoice()) = false) then
         begin
            ProgressPage.SetText('Granting the service account its folders...', 'On a large message store this takes a few minutes.');
            if (GrantServiceAccess() = false) then
               Result := false;
         end;

         // The administrator password, as the server's own PBKDF2 hash, before the
         // database step signs in with it and starts the service.
         ProgressPage.SetText('Setting the administrator password...', '');
         bPasswordWritten := WriteAdministratorPassword();
         if (bPasswordWritten = false) then
            Result := false;

         // Under hMailServer.Updater with the earlier version to put back
         // (g_szRollbackVersion), a step above that did not complete stops the
         // installation here, before the database step, its service not started: the
         // Updater puts the earlier version back on exit code 10, and the database
         // is then still as that version left it - the only database it can run on.
         // Carried on past such a step, the database step could upgrade the database
         // and the rollback leave the earlier version on a database it refuses: a
         // server with no mail. An installation run by hand, or with no version to
         // put back, carries on as it always did: the database step brings the
         // database to this version, which is then the one that can run on it.
         bStopBeforeDatabase := false;
         if (g_szRollbackVersion <> '') and (IsNewDatabase() = false) then
         begin
            szNotComplete := StepsNotCompleteBeforeDatabase(bRuntimeReady, bDatabaseBackendReady, bServiceRegistered, bPasswordWritten);
            if szNotComplete <> '' then
            begin
               bStopBeforeDatabase := true;
               Result := false;
               Log('The installation stops before the database step, which it does not run: ' + szNotComplete + ' did not complete.');
               SuppressibleMsgBox('This installation stopped before its database step, which it did not run: ' + szNotComplete + ' did not complete (the message above says why). ' +
                      'The database is as ' + PreviousVersionText() + ' left it, and hMailServer.Updater puts ' + PreviousVersionText() + ' back.', mbError, MB_OK, IDOK);
            end;
         end;

         ProgressPage.SetText('Initializing hMailServer database...', '');
         ProgressPage.SetProgress(4,6);

         szParameters := '';

         if (WizardSilent() = true) then
         begin
             szParameters:= '/silent';
         end;

         // Add the password as well, so that the administrator doesn't have to type it in
         // again if he has just entered it, or supplied it with /adminpassword or
         // HM_ADMIN_PASSWORD.
         //
         // This is load-bearing on the UPGRADE path, not just the create path. DBUpdater
         // must authenticate before it can move the schema, and until 6.2.23 DBSetupQuick
         // forwarded only /silent to it - so on a silent upgrade DBUpdater had no password,
         // fell through to a modal dialog nobody was there to answer, and hung the install
         // indefinitely. DBSetupQuick now forwards this value on both paths, and the shared
         // Authenticator fails instead of prompting under /silent, so the worst case is a
         // reported exit code rather than a wait with no end.
         //
         // Not when it could not be written above: the server would refuse it, the file
         // having no password, and the database step then signs in with none - which is
         // what such a server has, as the message just said.
         //
         // In DBSetupQuick's environment, HM_ADMIN_PASSWORD, as WriteAdministratorPassword
         // hands it to hMailServer.exe, emptied again as soon as DBSetupQuick has finished;
         // DBSetupQuick hands it on to DBUpdater the same way, and DBSetup, run instead
         // for an external database, is given it the same way. The live update's token
         // goes the same way, as "token:<hex>". Until 26 September 2026 the password was
         // ' password:' + the password on DBSetupQuick's (or DBSetup's) command line,
         // unquoted, and DBSetupQuick appended it to DBUpdater's the same way: a password
         // with a space reached the database tools cut at the space, a double quote or
         // backslashes before one changed it, and a new installation or an upgrade with
         // such a password failed with exit code 3 (DBSetup asked for it again).
         szCredential := '';
         if (Length(g_szAdminPassword) > 0) and bPasswordWritten then
            szCredential := g_szAdminPassword
         else if (Length(g_szUpgradeToken) > 0) then
            szCredential := 'token:' + g_szUpgradeToken;

         // Skipped when the installation stopped before it (bStopBeforeDatabase), and
         // when the built-in database engine could not be installed: the
         // tools cannot possibly succeed without it, and a second, vaguer error
         // dialog on top of the specific one from InstallSQLCE only obscures the
         // cause.
         if bStopBeforeDatabase or (bDatabaseBackendReady = false) then
         begin
            Result := false;
         end
         // Check both that the tool could be launched AND its exit code: a failed or
         // cancelled database create/upgrade must not masquerade as a successful
         // install (the service would run against a missing or outdated schema).
         else if ((GetCurrentDatabaseType() <> '') or g_bUseInternal) then
         begin
            // An existing database: from here it may change, until its version
            // afterwards shows it did not (BeginDatabaseStep, EndDatabaseStep).
            bDatabaseStepOnExisting := (IsNewDatabase() = false);
            if bDatabaseStepOnExisting then
               BeginDatabaseStep();

            // An existing database - an upgrade, or a repair over one - meets the
            // new binary here, before DBUpdater has moved it: the service is
            // started with the argument that says so (StartServiceForDatabaseUpgrade).
            // A new installation has no database for the service to find yet.
            if (IsNewDatabase() = false) and (bServiceRegistered = true) then
               StartServiceForDatabaseUpgrade();

            ResultCode := 0;
            bStarted := false;
            if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', szCredential) then
               SuppressibleMsgBox('The hMailServer database step could not be started: the administrator password could not be handed to DBSetupQuick.exe. Run DBSetupQuick.exe from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK)
            else
            begin
               try
                  bStarted := Exec(ExpandConstant('{app}\Bin\DBSetupQuick.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode);
                  if (bStarted = False) then
                     SuppressibleMsgBox(SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               finally
                  SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');
               end;
            end;

            // The database's version now, whatever the step's result: the step
            // can fail after it has moved it - a chain of steps on MySQL, whose DDL
            // commits as it goes, or the service's own start with UpgradeOnStart=1.
            if bDatabaseStepOnExisting then
               EndDatabaseStep();

            if (bStarted = False) then
            begin
               Result := false;
               if bDatabaseStepOnExisting and g_bDatabaseChanged then
                  SayDatabaseStepStoppedPartWay();
            end
            else if (ResultCode <> 0) then
            begin
               SuppressibleMsgBox('The hMailServer database could not be created or upgraded (exit code ' + IntToStr(ResultCode) + ').' #13#13
                      'The hMailServer service may not work until the database has been upgraded. Run DBSetupQuick.exe from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK);
               Result := false;
               if bDatabaseStepOnExisting and g_bDatabaseChanged then
                  SayDatabaseStepStoppedPartWay();
            end
            else
            begin
               // The database is there and at this build's schema: a new one's
               // secure defaults, then the profile the last page chose, and on a new
               // installation the stored-secrets key, before the service is started below.
               if IsNewDatabase() then
                  ProgressPage.SetText('Securing the new installation...', '');
               ApplyNewInstallDefaults(g_bUseInternal);
               ProgressPage.SetText('Applying the server profile...', '');
               ApplyChosenProfile();
               ProgressPage.SetText('Making the stored-secrets key...', '');
               CreateStoredSecretsKey();
            end;
         end
         else
         begin
            // DBSetup signs in with the same credential, the same way, so that the
            // administrator is not asked again for the password just typed: until
            // 26 September 2026 it had it on its command line with DBSetupQuick's.
            ResultCode := 0;
            bStarted := false;
            szDBSetupSaid := '';
            if g_bDatabaseGiven then
            begin
               // The database given with /dbtype or HM_DB_TYPE, set up with no
               // window (RunUnattendedDBSetup), silent installation or not.
               bStarted := RunUnattendedDBSetup(szCredential, ResultCode, szDBSetupSaid);
               if (bStarted = False) then
                  SuppressibleMsgBox('The hMailServer database setup could not be started. ' + szDBSetupSaid + #13#10#13#10 +
                         'Run DBSetup.exe from the hMailServer Bin folder to set the database up.', mbError, MB_OK, IDOK);
            end
            else if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', szCredential) then
               SuppressibleMsgBox('The hMailServer database setup could not be started: the administrator password could not be handed to DBSetup.exe. Run DBSetup.exe from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK)
            else
            begin
               try
                  bStarted := Exec(ExpandConstant('{app}\Bin\DBSetup.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode);
                  if (bStarted = False) then
                     SuppressibleMsgBox(SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               finally
                  SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');
               end;
            end;

            if (bStarted = False) then
            begin
               Result := false;
            end
            else if (ResultCode <> 0) and g_bDatabaseGiven then
            begin
               // DBSetup's own lines say which setting it refused, or what the
               // database server answered; they hold no password.
               SuppressibleMsgBox('The hMailServer database was not set up (DBSetup.exe /unattended, exit code ' + IntToStr(ResultCode) + '):' + #13#10#13#10 + szDBSetupSaid + #13#10#13#10 +
                      'The hMailServer service may not work until the database has been set up. Correct the database settings (docs/WindowsOnArm.md) and run DBSetup.exe from the hMailServer Bin folder, or run this setup again.', mbError, MB_OK, IDOK);
               Result := false;
            end
            else if (ResultCode <> 0) then
            begin
               SuppressibleMsgBox('The hMailServer database setup did not complete (exit code ' + IntToStr(ResultCode) + ').' #13#13
                      'The hMailServer service may not work until the database has been set up. Run DBSetup.exe from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK);
               Result := false;
            end
            else
            begin
               // An external database set up: a new one's secure defaults, the
               // profile the last page chose, as for the built-in one, and the
               // stored-secrets key - each of which leaves alone a database DBSetup
               // attached that is already in service: the defaults because only a
               // database DBSetup created carries the mark they require, the profile
               // by --if-new, the key because the verb refuses where that database
               // records one.
               if IsNewDatabase() then
                  ProgressPage.SetText('Securing the new installation...', '');
               ApplyNewInstallDefaults(false);
               ProgressPage.SetText('Applying the server profile...', '');
               ApplyChosenProfile();
               ProgressPage.SetText('Making the stored-secrets key...', '');
               CreateStoredSecretsKey();
            end;
         end;

         // The move to the virtual account, once the database is there.
         // Not after a database step that failed: the service stays LocalSystem's,
         // where DBSetup can be run again, and is moved from the Control Panel.
         if bServiceRegistered and Result and MoveServiceAfterDatabase(GetServiceAccountChoice()) then
         begin
            ProgressPage.SetText('Moving the service to NT SERVICE\hMailServer...', 'Granting its folders; on a large message store this takes a few minutes.');
            MoveServiceToVirtualAccount();
         end;

         ProgressPage.SetText('Starting the hMailServer service...', '');
         ProgressPage.SetProgress(5,6);

         // Only worth attempting if there is a service to start. The result of this
         // used to be discarded entirely, so an installation that produced no
         // working service still finished on "Completed".
         //
         // The verdict comes from the SCM rather than from net.exe's exit code,
         // which is ambiguous. Note what this does and does not prove: hMailServer
         // reports SERVICE_RUNNING before it connects to the database (ServiceMain
         // in hMailServer.cpp), so a running service means the process started, not
         // that the schema is usable. The schema is covered by the DBSetupQuick exit
         // code checked above.
         //
         // Not after the installation stopped before its database step: hMailServer.Updater
         // puts the earlier version back, and this one would meet a database it may
         // not run on.
         if bStopBeforeDatabase then
            Log('The hMailServer service is not started: the installation stopped before its database step.')
         else if (bServiceRegistered = true) then
         begin
            if (Exec(ExpandConstant('{sys}\net.exe'), 'START hMailServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
            begin
               SuppressibleMsgBox('The hMailServer service could not be started.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               Result := false;
            end
            else if (IsServiceRunning('hMailServer') = false) then
            begin
               SuppressibleMsgBox('The hMailServer service was installed but did not start.' + #13#10#13#10 +
                      'Check hMailServer.log and the ERROR log in the hMailServer Logs folder, then start the ' +
                      'service from services.msc or with "net start hMailServer".', mbError, MB_OK, IDOK);
               Result := false;
            end;
         end
         else
         begin
            Result := false;
         end;

         ProgressPage.SetText('Completed', '');
         ProgressPage.SetProgress(6,6);
      except
         // ssPostInstall is past the point at which Inno can roll anything back, so
         // an exception escaping from here abandons the wizard with the files
         // installed and - depending on how far it got - no service, no database and
         // nothing on screen saying which step failed or what to run by hand.
         SuppressibleMsgBox('A post-installation step failed:' + #13#10 + GetExceptionMessage + #13#10#13#10 +
                'The hMailServer program files are installed. To finish by hand, from an elevated prompt in the hMailServer Bin folder: ' +
                'run "hMailServer.exe --service-account localsystem /Register", then DBSetup.exe, then - to move the service to its own account - ' +
                '"hMailServer.exe --confirm /GrantServiceAccess" and "hMailServer.exe --service-account virtual /Register", then start the hMailServer service.', mbError, MB_OK, IDOK);
         Result := false;
      end;
   finally
     ProgressPage.Hide();
   end;

end;

function MoveIni() : Boolean;
  var sOldFile : AnsiString;
  var sNewFile : AnsiString;
begin

   CreateDir(ExpandConstant('{app}\Bin'));
   sOldFile := ExpandConstant('{win}\hMailServer.ini');
   sNewFile := ExpandConstant('{app}\Bin\hMailServer.ini');

   // Copy the file from the Windows directory
   // to the Bin directory. hMailServer uses the
   // file located in the Bin directory.
   if (FileCopy(sOldFile, sNewFile, True) = True) then
   begin
      // Rename the old hmailserver.ini
      sNewFile := sOldFile + '.old';
      if (FileCopy(sOldFile, sNewFile, True) = True) then
      begin
        // We've managed to backup hMailServer.ini in the
        // windows directory. Now delete the original.
        DeleteFile(sOldFile);
      end;
   end;
   Result := true;

end;

function CheckIsOldMySQLInstallation(szIniFile: AnsiString) : boolean;
var
   szDatabasePort : AnsiString;
   szProgramFolder: AnsiString;
   szMySQLExecutable : AnsiString;
   iFileSize: Integer;
   szMessage : AnsiString;
   szDatabase : AnsiString;
   szDatabaseHost : AnsiString;
   szDatabaseUsername : AnsiString;
begin

  // Assigned explicitly. This is the answer on every modern installation, and a
  // function whose True return aborts the wizard should not be leaving its result
  // to whatever the interpreter happens to initialise it to.
  Result := false;

  szDatabasePort := GetIniString('Database', 'Port', '', szIniFile);
  szDatabase := GetIniString('Database', 'Database', '', szIniFile);
  szDatabaseHost := GetIniString('Database', 'Server', '', szIniFile);
  szDatabaseUsername := GetIniString('Database', 'Username', '', szIniFile);
  szProgramFolder := RemoveBackslash(GetIniString('Directories', 'ProgramFolder', '', szIniFile));

  if (GetCurrentDatabaseType() = 'mysql') and
     (szDatabasePort = '3307') and
     (szDatabase = 'hMailServer') and
     (szDatabaseHost = 'localhost') and
     (szDatabaseUsername = 'root') then begin

    // We're using an internal MySQL database.
    szMySQLExecutable := szProgramFolder + '\MySQL\Bin\mysqld-nt.exe';

    // Check the size of MySQL.
    iFileSize := 0;
    if (FileSize(szMySQLExecutable, iFileSize)) then begin
       // MySQL in 4.4.3 is larger than 3500000 bytes.
       if (iFileSize < 3500000) then begin
          // MySQL is too old.

          szMessage := 'This version of hMailServer does not include MySQL. hMailServer can still' + #13 +
                       'use MySQL as backend though, assuming it is already installed on the system.' + #13 +
                       '' + #13 +
                       'However, the MySQL version hMailServer is configured to use is'  + #13 +
                       'too old for this version of hMailServer.' + #13 +
                       ''+ #13 +
                       'To solve this issue you must install the latest hMailServer 4 version' + #13 +
                       'before upgrading to hMailServer 5.' + #13
                       ''+ #13 +
                       'The latest hMailServer 4 version will upgrade MySQL to an version' + #13 +
                       'which is compatible with hMailServer 5.'

  		  	SuppressibleMsgBox(szMessage, mbError, MB_OK, IDOK)
          Result := true;
       end;
    end
    else
    begin
          szMessage := 'hMailServer 5 and later does not include MySQL. hMailServer 5 can still' + #13 +
                       'use MySQL as backend though, assuming it is already installed on the system.' + #13 +
                       '' + #13 +
                       'You have configured hMailServer 4 to use the bundled MySQL installation. However'+ #13 +
                       'hMailServer 4 with MySQL appears to have been uninstalled prior to running this' + #13 +
                       'hMailServer 5 installation. Hence, the MySQL installation hMailServer needs is' + #13 +
                       'no longer available.' + #13 +
                       '' + #13 +
                       'To solve this problem, reinstall the same hMailServer 4 version as before and then' + #13 +
                       'upgrade to version 5 without first uninstalling version 4.' + #13 +
                       '' + #13 +
                       'As an alternative, you can cancel this installation, delete the entire hMailServer ' + #13 +
                       'directory and then run this installation program again. Using this method, your configuration' + #13 +
                       'and email messages will be lost.';

  		  	SuppressibleMsgBox(szMessage, mbError, MB_OK, IDOK)
          Result := true;
    end;

  end;


end;

// The move page says, when the database is SQL Server reached with Windows
// authentication, that the moved service needs a login there. Read when the
// page is shown, because the settings file is found from the folder chosen on
// an earlier page.
procedure CurPageChanged(CurPageID: Integer);
var
   szSentence: String;
begin
   // Nested rather than joined with or: the page does not exist in a silent
   // installation, and its ID must not be read then.
   if (g_pageServiceAccount = nil) or g_bMssqlSentenceAdded then
      Exit;
   if (CurPageID <> g_pageServiceAccount.ID) then
      Exit;
   if IsNewService() or (IsLocalSystemAccount(GetServiceAccountBefore()) = false) then
      Exit;

   szSentence := MssqlIntegratedSentence();
   if szSentence <> '' then
      g_pageServiceAccount.SubCaptionLabel.Caption := g_pageServiceAccount.SubCaptionLabel.Caption + szSentence;
   g_bMssqlSentenceAdded := true;
end;

// Inno calls this after the last wizard page and before any file is installed, in a
// silent install too, and a non-empty answer ends the installation with that
// sentence (Setup's exit code 7). It is here and not in InitializeSetup because the
// settings file, which says whether this is a new installation, is found through
// {app}, which is not known until the directory has been chosen. Both installers:
// the database given for a new installation; the ARM64 one, below, what its server
// cannot use.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
   Result := '';

   // A new installation of the server given a database (DatabaseSwitch): its type
   // and the password file's path held to what DBSetup takes, before anything is
   // installed. DBSetup holds the rest to it, and refuses before it changes anything.
   if g_bDatabaseGiven and (GetCurrentDatabaseType() = '') and IsComponentSelected('server') then
   begin
      if NormalizedDatabaseType(g_szDatabaseType) = '' then
      begin
         Result := 'The database given with ' + g_szDatabaseFrom + ' is ''' + g_szDatabaseType + '''; it must be PostgreSQL, MySQL (or MariaDB) or MSSQL (Microsoft SQL Server). ' +
                   'The built-in database (x64 only) is used when neither /dbtype nor HM_DB_TYPE is given; /useinternaldbms does not override them. Nothing has been changed.';
         Exit;
      end;

      if (DatabaseSwitch('dbpasswordfile') <> '') and (ExtractFileDrive(DatabaseSwitch('dbpasswordfile')) = '') then
      begin
         Result := '/dbpasswordfile takes the full path of the file that holds the database password, for example /dbpasswordfile=C:\Deploy\hmailserver-db.txt. Nothing has been changed.';
         Exit;
      end;
   end;

#ifdef HM_ARM64
   // The ARM64 installer only (hMailServerARM64.iss).

   // The ARM64 server cannot open SQL Server Compact: there is no ARM64 build of it,
   // and an ARM64 process cannot load the x64 provider. Installed over a server that
   // uses the built-in database, it would leave that server unable to start.
   if (GetCurrentDatabaseType() = 'mssqlce') then
   begin
      Result := 'This hMailServer uses the built-in database (SQL Server Compact), which the ARM64 build cannot use. ' +
                'Move its data to Microsoft SQL Server or PostgreSQL with the x64 build first (MigratingDatabaseBackend.md in the documentation), ' +
                'then run this installer again. Nothing has been changed.';
      Exit;
   end;

   // A new installation of the server: with no built-in database, it is set up
   // against the database given with /dbtype or HM_DB_TYPE, by DBSetup /unattended,
   // or else in DBSetup's wizard - which is a window, and a silent installation has
   // nobody to answer one: under /VERYSILENT it would wait for ever. So a silent new
   // installation is refused here, before anything is installed, until it is told
   // which database to use.
   if (GetCurrentDatabaseType() = '') and IsComponentSelected('server') and WizardSilent() and (g_bDatabaseGiven = false) then
   begin
      Result := 'hMailServer on ARM64 has no built-in database, so a new installation is set up against Microsoft SQL Server or PostgreSQL, ' +
                'and a silent installation has to be told which: for example /dbtype=PostgreSQL /dbhost=HOST /dbuser=USER, with the password in ' +
                'HM_DB_PASSWORD or in the file /dbpasswordfile= names (WindowsOnArm.md in the documentation). Nothing has been changed.';
      Exit;
   end;

   // MySQL or MariaDB given for a new installation: the server cannot load an x64
   // client, and this installer carries no ARM64 one, so DBSetup would fail after
   // everything was installed. Refused here unless a libmysql.dll - which has to be
   // an ARM64 one - has been put in Bin already, as WindowsOnArm.md says.
   if (GetCurrentDatabaseType() = '') and IsComponentSelected('server') and g_bDatabaseGiven and (g_szDatabaseType = 'MySQL') and
      (FileExists(ExpandConstant('{app}\Bin\libmysql.dll')) = false) then
   begin
      Result := 'MySQL or MariaDB was given as the database, and the ARM64 build needs an ARM64 build of the MariaDB client, libmysql.dll, which this installer does not carry. ' +
                'Put one in ' + ExpandConstant('{app}\Bin') + ' first (WindowsOnArm.md in the documentation says how to build it), or use PostgreSQL or Microsoft SQL Server. Nothing has been changed.';
      Exit;
   end;

   // MySQL and MariaDB: the ARM64 server needs an ARM64 build of the client
   // (libmysql.dll), which this installer does not carry. A libmysql.dll an x64
   // installation left in Bin is the x64 one, and does not load either.
   if (GetCurrentDatabaseType() = 'mysql') then
      SuppressibleMsgBox('This hMailServer uses MySQL or MariaDB. The ARM64 build needs an ARM64 build of the MariaDB client, libmysql.dll, in its Bin folder; ' +
             'this installer does not carry one, and an x64 libmysql.dll does not load into the ARM64 server. ' +
             'The service will not start until an ARM64 one is there (WindowsOnArm.md in the documentation says how to build it).', mbInformation, MB_OK, IDOK);
#endif
end;

function NextButtonClick(CurPage : Integer): boolean;
var
   hWnd: Integer;
   szIniFile : AnsiString;

begin
	// We default to true.
	Result := true;

	if (CurPage = wpSelectDir) then
	begin
		szIniFile := GetIniFile();

		// Check if this folder contains an old MySQL installation, or if
		// the old MySQL installation has been uninstalled.
		if CheckIsOldMySQLInstallation(szIniFile) = true then 
		begin
			Result := false;
		end;
	end
	else if CurPage = wpReady then
	begin
		// The database's version as the server this installation replaces left it,
		// asked of that server while it still runs (ReadDatabaseVersion): what the
		// version after the database step is held to (EndDatabaseStep). Only from a
		// running service - a stopped one is not started for it - and only over an
		// existing installation's database.
		if IsComponentSelected('server') and (GetCurrentDatabaseType() <> '') then
		begin
			if IsServiceRunning('hMailServer') then
				g_iDatabaseVersionBefore := ReadDatabaseVersion('before this installation, as the running hMailServer service has it')
			else
				Log('The hMailServer service is not running, so the database''s version before this installation is not read.');
		end;

		// Stop the service before any program file is replaced, and refuse to go
		// past this page if it will not stop. See StopHMailServerService for why
		// that has to be a refusal rather than a warning, and for why the wait is
		// now bounded.
		if (StopHMailServerService() = false) then
			Result := false;
    end;
	
    hWnd := StrToInt(ExpandConstant('{wizardhwnd}'));

	if WizardSilent() = false then
	begin
		if CurPage = g_pageAccessKey.ID then
		begin
			// Check that passwords matches.
			if (Length(g_pageAccessKey.Values[0]) < 5) or (g_pageAccessKey.Values[0] <> g_pageAccessKey.Values[1]) then
			begin
				SuppressibleMsgBox('The two passwords must match and be at least 5 characters long.', mbError, MB_OK, IDOK)
				Result := false;
			end;

			g_szAdminPassword := g_pageAccessKey.Values[0];
		end;
	end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin

	if CurStep = ssInstall then
	begin
	   // Whether the database is new, asked of the settings file before the
	   // installation writes one (IsNewDatabase).
	   IsNewDatabase();

	   // And the account the service ran as, before this installation registers
	   // it again (GetServiceAccountBefore).
	   GetServiceAccountBefore();

	   // Move hMailServer.ini before files are copied
	   MoveIni();
	end;
	
	if CurStep = ssPostInstall then
	begin
	  // The Software\hMailServer InstallLocation value that used to be written here
	  // now lives in section_registry.iss, so that the uninstaller removes it and so
	  // that it is in place before the service is started below. The unused
	  // szIniFile assignment that followed it went with it.

  	// Create the hMailServer database
 	  if (IsComponentSelected('server')) then
	  begin
	    // The result used to be discarded here, so a failed database create or
	    // upgrade showed a (suppressible) message box and setup still finished
	    // with exit code 0 - a scripted deployment could not tell. The exception
	    // puts the failure in the log and on screen; the exit code, 10 or 11, is
	    // GetCustomSetupExitCode's, since Inno Setup goes on to exit 0 after an
	    // exception here. The specific message has already been shown by
	    // RunPostInstallTasks itself.
	    if (RunPostInstallTasks() = false) then
	    begin
	      g_bStepsIncomplete := true;
	      if g_bDatabaseChanged then
	        RaiseException('A post-installation step did not complete after the database step had changed the database, so ' + PreviousVersionText() + ' is not to be put back on it: it may not run on that database. See the message above.')
	      else if g_bGrantFailed then
	        RaiseException('The hMailServer service account could not be granted its folders. See the message above; run "hMailServer.exe --confirm /GrantServiceAccess" as an administrator from the hMailServer Bin folder to retry.')
	      else
	        RaiseException('A post-installation step did not complete: the administrator password, the service or the database. See the message above; run DBSetupQuick.exe from the hMailServer Bin folder to retry the database.');
	    end;
	  end
	 else
	 begin
	   if (IsComponentSelected('admintools')) then
	   begin
	      RegisterTypeLib();
	   end;
	 end;

   DeleteOldFiles();

	end;

end;

// Setup's exit code when it has run to the end, past Inno Setup's own 1 to 8: 0 when
// every step completed; when a step after the files did not complete
// (g_bStepsIncomplete), 10 while the database is as the version this installation
// replaced left it, and 11 once the database step has changed it, or could not be
// shown to have left it unchanged (g_bDatabaseChanged). hMailServer.Updater puts the
// earlier version back on 10 (InstallerStepsIncomplete) and never on 11
// (InstallerDatabaseUpgraded): the earlier version may not run on the database 11
// leaves, so a rollback then would leave a server that serves no mail. A scripted
// installation reads the same: 11 is "do not reinstall the earlier version
// without restoring the database".
function GetCustomSetupExitCode(): Integer;
begin
	if g_bStepsIncomplete and g_bDatabaseChanged then
		Result := 11
	else if g_bStepsIncomplete then
		Result := 10
	else
		Result := 0;
end;

// The uninstaller's own step before [UninstallRun] and before any file is deleted:
// Inno reaches usUninstall first. The service is stopped here and its process
// waited for (StopHMailServerAndWaitForExit), so that the [UninstallRun] entries
// and the file deletions after them meet a server that has gone, rather than one
// that has only said it is going: net stop alone left hMailServer.exe and six DLLs
// in Bin (section_uninstallrun.iss says how).
//
// When the process does not exit within the bounds, the uninstall carries on: the
// files the server loads are marked uninsrestartdelete (section_files_64.iss and
// section_files_common.iss), so Inno queues any it cannot delete for deletion when
// Windows next starts and asks for that restart itself, and this says why first -
// nothing is left behind without a word. Under /SUPPRESSMSGBOXES the note goes to
// the log only.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
   budgetMs: Integer;
begin
   if CurUninstallStep = usUninstall then
   begin
      budgetMs := GetServiceStopBudgetMs();

      if (StopHMailServerAndWaitForExit(budgetMs) = false) then
      begin
         Log('The hMailServer service or its process was still running when the files were to be removed; any file it still holds is queued for deletion when Windows next restarts (uninsrestartdelete).');
         SuppressibleMsgBox('The hMailServer service did not stop, or its process did not exit, within ' + IntToStr((budgetMs + PROCESS_EXIT_BUDGET_MS) div 1000) + ' seconds.' + #13#10#13#10 +
                'The uninstallation continues. A program file the server still holds open when its turn comes is queued ' +
                'for deletion when Windows next restarts, and the uninstaller asks whether to restart at the end.', mbInformation, MB_OK, IDOK);
      end;
   end;
end;


