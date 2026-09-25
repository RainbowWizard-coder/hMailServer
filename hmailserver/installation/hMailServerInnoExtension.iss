[code]
// GLOBAL
var
  g_pageAccessKey: TInputQueryWizardPage;
  g_szAdminPassword: String;
  g_szUpgradeToken: String;
  
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

function GetHashedPassword(Param: String) : String;
begin
  Result := GetMD5OfString(g_szAdminPassword);
end;

// Check function on the Security\AdministratorPassword entry in section_ini.iss.
//
// Without it, an installation that never collects a password - every silent
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
// /adminpassword=<password> (see InitializeSetup).
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
procedure InstallDotNetRuntime();
	var
		ResultCode: Integer;
begin
	if not DotNetDesktopMissing() then
		Exit;

	if (Exec(ExpandConstant('{tmp}\{#DOTNET_RUNTIME_FILE}'), '/install /quiet /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		SuppressibleMsgBox('The .NET {#DOTNET_MAJOR} Desktop Runtime could not be installed. The database setup tools will not work until it is installed.' + #13#10 +
		       SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
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

		   // ...and likewise when it came in on the command line, so that
		   // /adminpassword works for an attended install too.
		   if HasAdministratorPassword() then
		      Result := true;
	   end;
	
     // The profile page: a new installation of the server only.
     if (PageID = g_pageProfile.ID) then
     begin
       if (IsComponentSelected('server') = false) or (IsNewDatabase() = false) then
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

	if (Length(g_szAdminPassword) > 0) and (Length(g_szAdminPassword) < 5) then
	begin
		SuppressibleMsgBox('The password given with /adminpassword must be at least 5 characters long.', mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	if (FindWindowByWindowName('hMailServer Control Panel') > 0) then
	begin
		SuppressibleMsgBox('hMailServer Control Panel is started. You must close down this application before starting the installation.',mbInformation, MB_OK, IDOK);	
		Result := false;
		Exit;
	end;

	// Guard against a legacy hMailServer Administrator from an earlier version
	// still running (it is no longer shipped, but may linger on upgrades).
	if (FindWindowByWindowName('hMailServer Administrator') > 0) then
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
function RegisterHMailServerService() : Boolean;
	var
		ResultCode: Integer;
begin
	Result := true;

	// As with the type library: hMailServer.exe returns -1 when CreateService or
	// ChangeServiceConfig fails, and ignoring it meant an installation with no
	// service at all still ended on "Completed" - the following "net START
	// hMailServer" failed too, and its result was ignored as well.
	if (Exec(ExpandConstant('{app}\Bin\hMailServer.exe'), '/Register', '',  SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	if (ResultCode <> 0) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created (hMailServer.exe /Register returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
		       'Check the hMailServer error log, then run "hMailServer.exe /Register" as an administrator from the ' +
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


function DeleteOldFiles() : Boolean;
begin

   DeleteFile(ExpandConstant('{app}\Bin\Copyright.txt'));
   DeleteFile(ExpandConstant('{app}\Bin\License.txt'));
   DeleteFile(ExpandConstant('{app}\Bin\Sourcecode.txt'));

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


function RunPostInstallTasks() : Boolean;
   var
      ResultCode: Integer;
      ProgressPage : TOutputProgressWizardPage;
      szParameters: AnsiString;
      bDatabaseBackendReady: Boolean;
      bServiceRegistered: Boolean;
begin
   Result := true;

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
         InstallDotNetRuntime();

         ProgressPage.SetText('Initializing database backend...', '');
         ProgressPage.SetProgress(2,6);

         bDatabaseBackendReady := InstallSQLCE();

         ProgressPage.SetText('Creating the hMailServer service...', '');
         ProgressPage.SetProgress(3,6);

         bServiceRegistered := RegisterHMailServerService();

         ProgressPage.SetText('Initializing hMailServer database...', '');
         ProgressPage.SetProgress(4,6);

         szParameters := '';

         if (WizardSilent() = true) then
         begin
             szParameters:= '/silent';
         end;

         // Add the password as well, so that the administrator doesn't have to type it in
         // again if he has just entered it, or supplied it with /adminpassword.
         //
         // This is load-bearing on the UPGRADE path, not just the create path. DBUpdater
         // must authenticate before it can move the schema, and until 6.2.23 DBSetupQuick
         // forwarded only /silent to it - so on a silent upgrade DBUpdater had no password,
         // fell through to a modal dialog nobody was there to answer, and hung the install
         // indefinitely. DBSetupQuick now forwards this value on both paths, and the shared
         // Authenticator fails instead of prompting under /silent, so the worst case is a
         // reported exit code rather than a wait with no end.
         if (Length(g_szAdminPassword) > 0) then
            szParameters := szParameters + ' password:' + g_szAdminPassword
         else if (Length(g_szUpgradeToken) > 0) then
            szParameters := szParameters + ' password:token:' + g_szUpgradeToken;

         // Skipped when the built-in database engine could not be installed: the
         // tools cannot possibly succeed without it, and a second, vaguer error
         // dialog on top of the specific one from InstallSQLCE only obscures the
         // cause.
         if (bDatabaseBackendReady = false) then
         begin
            Result := false;
         end
         // Check both that the tool could be launched AND its exit code: a failed or
         // cancelled database create/upgrade must not masquerade as a successful
         // install (the service would run against a missing or outdated schema).
         else if ((GetCurrentDatabaseType() <> '') or g_bUseInternal) then
         begin
            // An existing database - an upgrade, or a repair over one - meets the
            // new binary here, before DBUpdater has moved it: the service is
            // started with the argument that says so (StartServiceForDatabaseUpgrade).
            // A new installation has no database for the service to find yet.
            if (IsNewDatabase() = false) and (bServiceRegistered = true) then
               StartServiceForDatabaseUpgrade();

            if (Exec(ExpandConstant('{app}\Bin\DBSetupQuick.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then
            begin
               SuppressibleMsgBox(SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               Result := false;
            end
            else if (ResultCode <> 0) then
            begin
               SuppressibleMsgBox('The hMailServer database could not be created or upgraded (exit code ' + IntToStr(ResultCode) + ').' #13#13
                      'The hMailServer service may not work until the database has been upgraded. Run DBSetupQuick.exe from the hMailServer Bin folder to retry.', mbError, MB_OK, IDOK);
               Result := false;
            end
            else
            begin
               // The database is there and at this build's schema: the profile the
               // last page chose, before the service is started below.
               ProgressPage.SetText('Applying the server profile...', '');
               ApplyChosenProfile();
            end;
         end
         else
         begin
            if (Exec(ExpandConstant('{app}\Bin\DBSetup.exe'), szParameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) = False) then
            begin
               SuppressibleMsgBox(SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
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
               // An external database set up: the profile the last page chose, as for
               // the built-in one - with --if-new, since DBSetup can attach a
               // database that is already in service.
               ProgressPage.SetText('Applying the server profile...', '');
               ApplyChosenProfile();
            end;
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
         if (bServiceRegistered = true) then
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
                'The hMailServer program files are installed. To finish by hand, from the hMailServer Bin folder: ' +
                'run "hMailServer.exe /Register", then DBSetup.exe, then start the hMailServer service.', mbError, MB_OK, IDOK);
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
	    // with exit code 0 - a scripted deployment could not tell. Raising turns
	    // it into a failed install with a non-zero exit code; the specific
	    // message has already been shown by RunPostInstallTasks itself.
	    if (RunPostInstallTasks() = false) then
	      RaiseException('The hMailServer database could not be created or upgraded. See the message above; run DBSetupQuick.exe from the hMailServer Bin folder to retry.');
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


