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
  g_bNewService: Boolean;
  g_bNewServiceKnown: Boolean;

  // Set when the service account could not be granted its folders on an
  // installation whose service already runs as something other than
  // LocalSystem - the one grant failure that fails the installation, and says
  // so in its own words rather than the database's.
  g_bGrantFailed: Boolean;
  g_bMssqlSentenceAdded: Boolean;

  // Set when the move to NT SERVICE\hMailServer was not made, or was taken back,
  // because that account cannot open the database: SQL Server reached with
  // Windows authentication on which it could not be given a login, a user and
  // db_owner (GrantServiceDatabaseLogin), or a moved service that did not open
  // its database (CheckServiceAfterMove). The service runs as LocalSystem, and
  // the installation fails in words of its own rather than ending with 0 on a
  // server that cannot serve mail.
  g_bDatabaseLoginFailed: Boolean;
  // What hMailServer.exe --grant-database-login said when it could not give the
  // account its login, for the message of a move taken back.
  g_szDatabaseLoginSaid: String;
  // Set when this installation's DBSetup /unattended created the database - not
  // attached one with /createdatabase=0, and not the wizard, whose choice is not
  // known here: a database no login of the virtual account can have reached yet.
  g_bDatabaseCreatedHere: Boolean;
  // Set when --apply-new-install-defaults or --create-stored-secrets-key, run as
  // the administrator running setup, could not open the database - SQL Server
  // with Windows authentication, where LocalSystem is a SQL Server administrator
  // and that person is not: the service is then started with the argument that
  // has it apply them itself ("sc start hMailServer --apply-new-install-defaults",
  // FinishNewInstallationAtStart in hMailServer.cpp).
  g_bFinishNewInstallationAtStart: Boolean;

  // Set when the database step was DBSetup's - a new installation's external
  // database - so that the last message names what retries it: this setup again,
  // or DBSetup. DBSetupQuick has nothing to retry there, and on an
  // installation without SQL Server Compact it cannot even start.
  g_bDatabaseStepByDBSetup: Boolean;

  // A new installation that an earlier run of this setup left unfinished
  // (PendingNewInstallation): its database step did not complete, so neither did
  // the steps after it - the secure defaults, the profile, the stored-secrets key
  // and the move to NT SERVICE\hMailServer. Read once, before this installation
  // clears it; g_bNewInstallationFinished is set once this one has run them.
  g_bPendingKnown: Boolean;
  g_bPendingBefore: Boolean;
  g_szPendingAccount: String;
  g_szPendingProfile: String;
  g_bNewInstallationFinished: Boolean;

  // Why the last ReadDatabaseVersion could not read the database's version.
  g_szLastDatabaseReadError: String;

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

  // Set at ssInstall when the program folder is not there yet: this installation
  // makes it, and gives it its permissions before anything is copied into it
  // (HardenNewProgramFolder, which [Dirs] runs once it has made the folder).
  g_bProgramFolderNew: Boolean;
  // The last folder CheckProgramFolderPermissions looked at, so that one choice
  // is said once.
  g_szProgramFolderChecked: String;

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
	// What GetAclInformation answers for AclSizeInformation: the number of entries
	// in a list of permissions, and its bytes in use and free (CheckProgramFolderPermissions).
	ACL_SIZE_INFO = record
		AceCount					: cardinal;
		AclBytesInUse				: cardinal;
		AclBytesFree				: cardinal;
	end;
	// The first eight bytes of an entry in such a list: its type, flags and size in
	// one cardinal (the type in the low byte, the flags in the next), and an allow
	// or deny entry's rights. The entry's SID follows them.
	ACE_START = record
		TypeFlagsSize				: cardinal;
		Mask						: cardinal;
	end;
	
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

	// For reading a folder's permissions (CheckProgramFolderPermissions), named
	// apart from the Windows names (SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
	// AclSizeInformation, ACCESS_ALLOWED_ACE_TYPE and the flags) for the reason the
	// two above are. HM_FOLDER_WRITE_RIGHTS is every right that changes what a
	// folder or a file holds, what it is called or who may change it -
	// AdminUpdateChecks::WriteRights, the rule an administrator's install of an
	// update holds the program's folders to - with GENERIC_ALL and GENERIC_WRITE,
	// which an entry may carry unmapped.
	HM_FILE_OBJECT              = 1;
	HM_DACL_INFORMATION         = 4;
	HM_ACL_SIZE_INFORMATION     = 2;
	HM_ACCESS_ALLOWED           = 0;
	HM_OBJECT_INHERIT           = $01;
	HM_CONTAINER_INHERIT        = $02;
	HM_NO_PROPAGATE_INHERIT     = $04;
	HM_INHERIT_ONLY             = $08;
	HM_FOLDER_WRITE_RIGHTS      = $500D0156;
	// Reading a file or listing a folder, or GENERIC_ALL unmapped
	// (EveryUserMayRead).
	HM_FOLDER_READ_RIGHTS       = $10000001;
	// For hardening a program folder that was there before a Control Panel
	// installation (HardenExistingProgramFolders): a folder held open with
	// FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES - a handle that
	// asks for the attributes alone takes no part in Windows' check of sharing, and
	// its refusal to share deleting would bind nobody (ProtectedFile.cpp's
	// HeldFolder holds a folder the same way) - shared for reading and writing but
	// not for deleting (FILE_SHARE_READ | FILE_SHARE_WRITE), OPEN_EXISTING, and
	// FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, so that a link is
	// opened itself, not followed; FILE_ATTRIBUTE_REPARSE_POINT; the owner alone
	// (OWNER_SECURITY_INFORMATION); and the owner with a list that inherits nothing
	// (OWNER_ | DACL_ | PROTECTED_DACL_SECURITY_INFORMATION).
	HM_HOLD_FOLDER_ACCESS       = $A1;
	HM_SHARE_ALL_BUT_DELETE     = $3;
	HM_OPEN_EXISTING            = 3;
	HM_HOLD_FOLDER_FLAGS        = $02200000;
	HM_REPARSE_POINT            = $400;
	HM_OWNER_INFORMATION        = 1;
	HM_OWNER_AND_PROTECTED_DACL = $80000005;

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

// A folder's permissions, read (CheckProgramFolderPermissions): GetNamedSecurityInfoW
// for its list, GetAclInformation and GetAce to walk it, RtlMoveMemory to copy the
// start of an entry out, ConvertStringSidToSidW and EqualSid to know the group it
// names, LocalFree for what Windows allocated. Pointers are cardinals, as HANDLE is:
// this setup is a 32-bit program. A String reaches a W function as a pointer to its
// wide characters. Named apart from the API for the reason SetProcessEnvironmentVariable is.
function ReadFolderPermissions(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; ppsidOwner: cardinal; ppsidGroup: cardinal;
                               var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal;
external 'GetNamedSecurityInfoW@advapi32.dll stdcall';
function ReadAclSize(pAcl: cardinal; var AclInformation: ACL_SIZE_INFO; nAclInformationLength: cardinal; dwAclInformationClass: cardinal): Boolean;
external 'GetAclInformation@advapi32.dll stdcall';
function ReadAclEntry(pAcl: cardinal; dwAceIndex: cardinal; var pAce: cardinal): Boolean;
external 'GetAce@advapi32.dll stdcall';
procedure CopyAceStart(var Destination: ACE_START; Source: cardinal; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
function SidOfText(StringSid: String; var Sid: cardinal): Boolean;
external 'ConvertStringSidToSidW@advapi32.dll stdcall';
function SidsAreEqual(Sid1: cardinal; Sid2: cardinal): Boolean;
external 'EqualSid@advapi32.dll stdcall';
function FreeWindowsMemory(hMem: cardinal): cardinal;
external 'LocalFree@kernel32.dll stdcall';

// A program folder that was there before a Control Panel installation, hardened in
// place (HardenExistingProgramFolders): CreateFileW to hold each folder from the
// drive's root down while it is looked at and changed, CloseHandle to let it go -
// as Integer, so that INVALID_HANDLE_VALUE reads -1 - GetFileAttributesW to know a
// link, GetNamedSecurityInfoW for the owner, and ConvertStringSecurityDescriptorToSecurityDescriptorW,
// GetSecurityDescriptorDacl, GetSecurityDescriptorOwner and SetNamedSecurityInfoW
// to give a folder a user owned a new owner and list. BOOL out parameters are
// cardinals, which is their width. Named apart from the API for the reason
// SetProcessEnvironmentVariable is.
function HoldFolder(lpFileName: String; dwDesiredAccess: cardinal; dwShareMode: cardinal; lpSecurityAttributes: cardinal;
                    dwCreationDisposition: cardinal; dwFlagsAndAttributes: cardinal; hTemplateFile: cardinal): Integer;
external 'CreateFileW@kernel32.dll stdcall';
function ReleaseFolder(hObject: Integer): Boolean;
external 'CloseHandle@kernel32.dll stdcall';
function FolderAttributes(lpFileName: String): cardinal;
external 'GetFileAttributesW@kernel32.dll stdcall';
function ReadFolderOwner(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; var ppsidOwner: cardinal; ppsidGroup: cardinal;
                         ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal;
external 'GetNamedSecurityInfoW@advapi32.dll stdcall';
function SecurityDescriptorOfText(StringSecurityDescriptor: String; StringSDRevision: cardinal; var SecurityDescriptor: cardinal; SecurityDescriptorSize: cardinal): Boolean;
external 'ConvertStringSecurityDescriptorToSecurityDescriptorW@advapi32.dll stdcall';
function DaclOfDescriptor(pSecurityDescriptor: cardinal; var lpbDaclPresent: cardinal; var pDacl: cardinal; var lpbDaclDefaulted: cardinal): Boolean;
external 'GetSecurityDescriptorDacl@advapi32.dll stdcall';
function OwnerOfDescriptor(pSecurityDescriptor: cardinal; var pOwner: cardinal; var lpbOwnerDefaulted: cardinal): Boolean;
external 'GetSecurityDescriptorOwner@advapi32.dll stdcall';
function WriteFolderSecurity(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal;
                             pDacl: cardinal; pSacl: cardinal): cardinal;
external 'SetNamedSecurityInfoW@advapi32.dll stdcall';

// The groups that hold every user of the computer, as SIDs - Everyone,
// Authenticated Users, Users and the rest of ServiceAccount.cpp's BroadSids_ that a
// folder's permissions name in practice - with the name each is given in what
// setup says. Empty past the last.
function BroadGroupSid(Index: Integer; var Name: String): String;
begin
   Result := '';
   Name := '';
   case Index of
      0: begin Result := 'S-1-1-0'; Name := 'Everyone'; end;
      1: begin Result := 'S-1-5-11'; Name := 'Authenticated Users'; end;
      2: begin Result := 'S-1-5-32-545'; Name := 'Users'; end;
      3: begin Result := 'S-1-5-4'; Name := 'INTERACTIVE'; end;
      4: begin Result := 'S-1-2-0'; Name := 'LOCAL'; end;
      5: begin Result := 'S-1-5-32-546'; Name := 'Guests'; end;
      6: begin Result := 'S-1-5-32-547'; Name := 'Power Users'; end;
      7: begin Result := 'S-1-5-32-555'; Name := 'Remote Desktop Users'; end;
      8: begin Result := 'S-1-5-14'; Name := 'REMOTE INTERACTIVE LOGON'; end;
      9: begin Result := 'S-1-5-13'; Name := 'TERMINAL SERVER USER'; end;
   end;
end;

// The name of the broad group (BroadGroupSid) that may change what an installation
// in Folder would hold, or '' when none may or the permissions cannot be read. Read
// on Folder itself when it is there - an allow entry that applies to it - and
// otherwise on the nearest folder above it that is there - an allow entry a folder
// made in it would inherit: one handed down to folders, or to files and not only
// to that folder's own. Where is the folder whose permissions were read. What
// counts is HM_FOLDER_WRITE_RIGHTS. Deny entries are not weighed: they can only
// take away, and a warning that is not needed costs less than one that is missed.
function BroadGroupThatMayWrite(Folder: String; var Where: String): String;
var
   szPath: String;
   szParent: String;
   szSid: String;
   szName: String;
   bExists: Boolean;
   bLooking: Boolean;
   bApplies: Boolean;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pAce: cardinal;
   pBroad: cardinal;
   info: ACL_SIZE_INFO;
   head: ACE_START;
   aceType: cardinal;
   aceFlags: cardinal;
   index: cardinal;
   group: Integer;
begin
   Result := '';
   Where := '';
   szPath := RemoveBackslashUnlessRoot(Folder);
   bExists := DirExists(szPath);
   bLooking := true;
   while bLooking and not DirExists(szPath) do
   begin
      szParent := ExtractFileDir(szPath);
      if (szParent = '') or (CompareText(szParent, szPath) = 0) then
         bLooking := false
      else
         szPath := szParent;
   end;
   if not DirExists(szPath) then
      Exit;
   Where := szPath;

   pDescriptor := 0;
   pDacl := 0;
   if ReadFolderPermissions(szPath, HM_FILE_OBJECT, HM_DACL_INFORMATION, 0, 0, pDacl, 0, pDescriptor) <> 0 then
      Exit;

   if pDacl = 0 then
      Result := 'Everyone (the folder has no list of permissions at all)'
   else if ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      index := 0;
      while (Result = '') and (index < info.AceCount) do
      begin
         pAce := 0;
         if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
         begin
            CopyAceStart(head, pAce, 8);
            aceType := head.TypeFlagsSize and $FF;
            aceFlags := (head.TypeFlagsSize shr 8) and $FF;
            if bExists then
               bApplies := (aceFlags and HM_INHERIT_ONLY) = 0
            else
               bApplies := ((aceFlags and HM_CONTAINER_INHERIT) <> 0) or
                           (((aceFlags and HM_OBJECT_INHERIT) <> 0) and ((aceFlags and HM_NO_PROPAGATE_INHERIT) = 0));
            if (aceType = HM_ACCESS_ALLOWED) and bApplies and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) then
            begin
               group := 0;
               szSid := BroadGroupSid(group, szName);
               while (Result = '') and (szSid <> '') do
               begin
                  pBroad := 0;
                  if SidOfText(szSid, pBroad) then
                  begin
                     if SidsAreEqual(pAce + 8, pBroad) then
                        Result := szName;
                     FreeWindowsMemory(pBroad);
                  end;
                  group := group + 1;
                  szSid := BroadGroupSid(group, szName);
               end;
            end;
         end;
         index := index + 1;
      end;
   end;

   FreeWindowsMemory(pDescriptor);
end;

// Says when a group that holds every user of the computer - Authenticated Users,
// whose Modify the root of a Windows client's drive hands every folder made there,
// or Users, whom Windows Server's lets make files and folders - may change what an
// installation in Folder would hold: the programs the service and the administrators
// run. A warning, never a refusal: setup hardens the program folder, Bin and
// ControlPanel (HardenNewProgramFolder for a folder it makes, and hMailServer.exe
// /ProtectFolders after the files, docs/ServiceAccount.md), and a folder made for
// the purpose off a drive's root is a choice an administrator may make. In a box
// when Show is set and the installation is attended - the folder page - and in the
// setup log always; once for a folder.
procedure CheckProgramFolderPermissions(Folder: String; Show: Boolean);
var
   szWho: String;
   szWhere: String;
   szText: String;
begin
   if CompareText(Folder, g_szProgramFolderChecked) = 0 then
      Exit;
   g_szProgramFolderChecked := Folder;

   szWho := BroadGroupThatMayWrite(Folder, szWhere);
   if szWho = '' then
      Exit;

   if CompareText(RemoveBackslashUnlessRoot(Folder), szWhere) = 0 then
      szText := Folder + ' lets ' + szWho + ' change what is in it'
   else
      szText := szWhere + ', the folder ' + Folder + ' would be made in, lets ' + szWho + ' change what is in a folder made there';
   szText := szText + ', so any account of the computer could replace hMailServer''s programs, which the service and the administrators run. ' +
             'Setup takes that right away from the program folder, Bin and ControlPanel - before it copies anything into a folder it makes, and with ' +
             'hMailServer.exe /ProtectFolders after the files - but a folder made for programs, such as the default ' + ExpandConstant('{commonpf}') + '\hMailServer, ' +
             'is the safer place.';
   Log('The program folder: ' + szText);
   if Show and not WizardSilent() then
      SuppressibleMsgBox(szText, mbInformation, MB_OK, IDOK);
end;

// Whether Everyone, Authenticated Users or Users may read what is in Folder, by an
// allow entry that applies to it (BroadGroupSid's first three): what
// ServiceAccount::Harden asks before it gives BUILTIN\Users Read & execute, so that
// a folder made in one only the administrators may open does not become every
// user's to read. True when it has no list at all; false when it cannot be read.
function EveryUserMayRead(Folder: String): Boolean;
var
   szSid: String;
   szName: String;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pAce: cardinal;
   pBroad: cardinal;
   info: ACL_SIZE_INFO;
   head: ACE_START;
   index: cardinal;
   group: Integer;
begin
   Result := false;
   pDescriptor := 0;
   pDacl := 0;
   if ReadFolderPermissions(Folder, HM_FILE_OBJECT, HM_DACL_INFORMATION, 0, 0, pDacl, 0, pDescriptor) <> 0 then
      Exit;

   if pDacl = 0 then
      Result := true
   else if ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      index := 0;
      while (not Result) and (index < info.AceCount) do
      begin
         pAce := 0;
         if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
         begin
            CopyAceStart(head, pAce, 8);
            if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERIT_ONLY) = 0) and
               ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then
            begin
               group := 0;
               while (not Result) and (group < 3) do
               begin
                  szSid := BroadGroupSid(group, szName);
                  pBroad := 0;
                  if SidOfText(szSid, pBroad) then
                  begin
                     if SidsAreEqual(pAce + 8, pBroad) then
                        Result := true;
                     FreeWindowsMemory(pBroad);
                  end;
                  group := group + 1;
               end;
            end;
         end;
         index := index + 1;
      end;
   end;

   FreeWindowsMemory(pDescriptor);
end;

// The program folder of a new installation, given the permissions hMailServer.exe
// /ProtectFolders gives it (ServiceAccount::Harden) as soon as [Dirs] has made it,
// before anything is copied into it: SYSTEM and the Administrators full control,
// BUILTIN\Users Read & execute where every user could read the folder as it was made
// (EveryUserMayRead), handed down, and nothing the folder above hands
// down - by SID, which no language of Windows changes. A folder made at a drive's
// root inherits Authenticated Users' Modify on a Windows client and Users' right to
// make files and folders on Windows Server, so every program copied into it would be
// one any user could change until /ProtectFolders runs after the files. Only a folder
// this installation made: one that was there keeps what is in it as it was until
// /ProtectFolders, which also gives it to the Administrators when a user owns it.
// When icacls cannot be run, or fails, the setup log says so and /ProtectFolders
// still hardens the folder after the files.
procedure HardenNewProgramFolder();
var
   ResultCode: Integer;
   szFolder: String;
   szUsers: String;
   szSaid: String;
begin
   szFolder := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
   if (not g_bProgramFolderNew) or (Length(szFolder) <= 3) then
      Exit;

   szUsers := '';
   szSaid := 'no entry for Users, as no user could read it as it was made';
   if EveryUserMayRead(szFolder) then
   begin
      szUsers := ' *S-1-5-32-545:(OI)(CI)RX';
      szSaid := 'Users Read & execute';
   end;

   if Exec(ExpandConstant('{sys}\icacls.exe'), '"' + szFolder + '" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers,
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      Log('icacls gave the new program folder ' + szFolder + ' its permissions - SYSTEM and the Administrators full control, ' + szSaid + ', nothing inherited - before the files, and returned ' + IntToStr(ResultCode) + '.')
   else
      Log('icacls could not be run to give the new program folder ' + szFolder + ' its permissions before the files: ' + SysErrorMessage(ResultCode) + '. hMailServer.exe /ProtectFolders gives them after the files.');
end;

// Whether the owner of Folder is SYSTEM, the Administrators or TrustedInstaller -
// ServiceAccount.cpp's IsTrustedWriter_, which ServiceAccount::Harden asks of a
// program folder: any other owner may change the permissions again, whatever they
// say. Known is false when the owner cannot be read.
function FolderOwnerIsTrusted(Folder: String; var Known: Boolean): Boolean;
var
   szSid: String;
   pOwner: cardinal;
   pDescriptor: cardinal;
   pTrusted: cardinal;
   index: Integer;
begin
   Result := false;
   Known := false;
   pOwner := 0;
   pDescriptor := 0;
   if ReadFolderOwner(Folder, HM_FILE_OBJECT, HM_OWNER_INFORMATION, pOwner, 0, 0, 0, pDescriptor) <> 0 then
      Exit;

   Known := pOwner <> 0;
   index := 0;
   while Known and (not Result) and (index < 3) do
   begin
      szSid := 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464';
      if index = 0 then
         szSid := 'S-1-5-18'
      else if index = 1 then
         szSid := 'S-1-5-32-544';
      pTrusted := 0;
      if SidOfText(szSid, pTrusted) then
      begin
         if SidsAreEqual(pOwner, pTrusted) then
            Result := true;
         FreeWindowsMemory(pTrusted);
      end;
      index := index + 1;
   end;

   FreeWindowsMemory(pDescriptor);
end;

// Folder held open for HardenExistingProgramFolders, or -1, with Why, when it
// cannot be or is a link: opened as itself, not followed, for listing as well as
// its attributes, shared for reading and writing but not for deleting - so that
// nobody can rename it, or put a link in its place, until it is let go: Windows
// holds a handle that asks for the attributes alone to no sharing at all - and then
// its attributes read by its path, which names the folder held.
function HoldProgramFolder(Folder: String; var Why: String): Integer;
var
   attributes: cardinal;
begin
   Why := '';
   Result := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   if Result = -1 then
   begin
      Why := Folder + ' could not be held open (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;

   // INVALID_FILE_ATTRIBUTES has every bit set, the link's among them.
   attributes := FolderAttributes(Folder);
   if (attributes and HM_REPARSE_POINT) <> 0 then
   begin
      ReleaseFolder(Result);
      Result := -1;
      Why := Folder + ' is a link - a junction, a symbolic link or a mount point - or its attributes cannot be read';
   end;
end;

// One folder of an installation of the Control Panel, there before it, hardened in
// place while it is held (HardenExistingProgramFolders), as ServiceAccount::Harden
// hardens a server's: SYSTEM and the Administrators full control and BUILTIN\Users
// Read & execute where every user could read it before (EveryUserMayRead), handed
// down, and nothing inherited. Owned by SYSTEM, the Administrators or
// TrustedInstaller, it keeps the entries of its own, as Harden keeps them - icacls
// on the SIDs. Owned by anybody else - a user who made the folder at a drive's root
// before the installation - it is given the Administrators as owner and a list of
// those entries alone, as Harden drops the entries of its own that let anybody else
// change it: that owner could have written them, and could change the permissions
// again. True when it was hardened.
function HardenHeldFolder(Folder: String): Boolean;
var
   ResultCode: Integer;
   bKnown: Boolean;
   szUsers: String;
   szSaid: String;
   szDescriptor: String;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pOwner: cardinal;
   present: cardinal;
   defaulted: cardinal;
   written: cardinal;
begin
   Result := false;
   szUsers := '';
   szDescriptor := 'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';
   szSaid := 'no entry for Users, as no user could read it';
   if EveryUserMayRead(Folder) then
   begin
      szUsers := ' *S-1-5-32-545:(OI)(CI)RX';
      szDescriptor := szDescriptor + '(A;OICI;0x1200a9;;;BU)';
      szSaid := 'Users Read & execute';
   end;

   if FolderOwnerIsTrusted(Folder, bKnown) then
   begin
      if Exec(ExpandConstant('{sys}\icacls.exe'), '"' + Folder + '" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers,
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      begin
         Result := ResultCode = 0;
         Log('icacls hardened ' + Folder + ' - SYSTEM and the Administrators full control, ' + szSaid + ', nothing inherited, the entries of its own kept - and returned ' + IntToStr(ResultCode) + '.');
      end
      else
         Log('icacls could not be run to harden ' + Folder + ': ' + SysErrorMessage(ResultCode) + '.');
      Exit;
   end;

   if not bKnown then
   begin
      Log('The owner of ' + Folder + ' could not be read, so its permissions were not changed.');
      Exit;
   end;

   pDescriptor := 0;
   if not SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0) then
   begin
      Log('The permissions for ' + Folder + ' could not be made (Windows error ' + IntToStr(DLLGetLastError()) + '), so they were not changed.');
      Exit;
   end;

   pDacl := 0;
   pOwner := 0;
   present := 0;
   defaulted := 0;
   if DaclOfDescriptor(pDescriptor, present, pDacl, defaulted) and OwnerOfDescriptor(pDescriptor, pOwner, defaulted) and (pDacl <> 0) and (pOwner <> 0) then
   begin
      written := WriteFolderSecurity(Folder, HM_FILE_OBJECT, HM_OWNER_AND_PROTECTED_DACL, pOwner, 0, pDacl, 0);
      Result := written = 0;
      if Result then
         Log(Folder + ' was owned by an account other than SYSTEM, the Administrators or TrustedInstaller, who could have changed what is in it and its permissions again: ' +
             'it is now the Administrators'', with SYSTEM and the Administrators full control, ' + szSaid + ', nothing inherited, and no entry of its own besides.')
      else
         Log('The permissions of ' + Folder + ', owned by an account other than SYSTEM, the Administrators or TrustedInstaller, could not be written (Windows error ' + IntToStr(written) + '), so they are as they were.');
   end
   else
      Log('The permissions made for ' + Folder + ' could not be read back, so they were not changed.');

   FreeWindowsMemory(pDescriptor);
end;

// The program folder of an installation of the Control Panel without the server
// (the controlpanel component, with or without admintools) when the folder was there
// before it: hardened at ssInstall, before the files, as a server installation's is
// by hMailServer.exe /ProtectFolders after its files, and a folder this installation
// makes by HardenNewProgramFolder before them. Such an installation has no server,
// so no /ProtectFolders runs, and a Control Panel installed into a folder an
// administrator had made at a drive's root kept the Authenticated Users' Modify that
// folder inherited on hMailCP.exe and the libraries beside it, which the
// administrators run. The program folder, then ControlPanel and Bin where they are
// there - each may have permissions of its own that the program folder's do not
// reach - each by HardenHeldFolder. Left as it is, and said in the setup log:
//   - a drive's root, as HardenNewProgramFolder leaves one;
//   - a folder that holds a server installation - Data, or Bin\hMailServer.INI -
//     which that installation's /ProtectFolders hardens once it has made Data and
//     the rest private, so that the program folder's change is not handed down
//     through the message store;
//   - a folder that is a link - a junction, a symbolic link, a mount point - or is
//     in one, or that cannot be held: its permissions would be written wherever the
//     link leads.
// Every folder from the one below the drive's root down to the one being hardened is
// held (HoldProgramFolder) from before it is looked at until after it is changed:
// at a drive's root on a Windows client any user may rename a folder that inherits
// Authenticated Users' Modify, and put a link in its place.
procedure HardenExistingProgramFolders();
var
   held: array of Integer;
   chain: array of String;
   szApp: String;
   szPath: String;
   szParent: String;
   szSub: String;
   szWhy: String;
   handle: Integer;
   count: Integer;
   index: Integer;
   bDone: Boolean;
begin
   szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
   if g_bProgramFolderNew or IsComponentSelected('server') or (Length(szApp) <= 3) or (not DirExists(szApp)) then
      Exit;
   if DirExists(AddBackslash(szApp) + 'Data') or FileExists(AddBackslash(szApp) + 'Bin\hMailServer.INI') then
   begin
      Log('The program folder ' + szApp + ' holds a server installation, so this installation of the Control Panel alone does not change its permissions: ' +
          'hMailServer.exe /ProtectFolders, run as an administrator from its Bin folder, hardens it.');
      Exit;
   end;

   // The folders from the program folder up to the one below the drive's root.
   count := 0;
   szPath := szApp;
   bDone := false;
   while not bDone do
   begin
      SetArrayLength(chain, count + 1);
      chain[count] := szPath;
      count := count + 1;
      szParent := ExtractFileDir(szPath);
      bDone := (szParent = '') or (CompareText(szParent, szPath) = 0) or (Length(szParent) <= 3);
      szPath := szParent;
   end;

   // Held from the top down, each looked at once it is held.
   SetArrayLength(held, 0);
   szWhy := '';
   index := count - 1;
   while (szWhy = '') and (index >= 0) do
   begin
      handle := HoldProgramFolder(chain[index], szWhy);
      if handle <> -1 then
      begin
         SetArrayLength(held, GetArrayLength(held) + 1);
         held[GetArrayLength(held) - 1] := handle;
      end;
      index := index - 1;
   end;

   if szWhy <> '' then
      Log('The program folder ' + szApp + ' was not hardened: ' + szWhy + '. Its permissions are as they were; docs/ServiceAccount.md gives the icacls lines that harden it by hand.')
   else
   begin
      HardenHeldFolder(szApp);
      for index := 0 to 1 do
      begin
         szSub := AddBackslash(szApp) + 'ControlPanel';
         if index = 1 then
            szSub := AddBackslash(szApp) + 'Bin';
         if DirExists(szSub) then
         begin
            handle := HoldProgramFolder(szSub, szWhy);
            if handle = -1 then
               Log(szSub + ' was not hardened: ' + szWhy + '. Its permissions are as they were.')
            else
            begin
               HardenHeldFolder(szSub);
               ReleaseFolder(handle);
            end;
         end;
      end;
   end;

   for index := 0 to GetArrayLength(held) - 1 do
      ReleaseFolder(held[index]);
end;

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
// exactly the installations that use it. 90 seconds on top of the configured
// drain period: the minute the stop gives the message indexer's thread when it
// is in a statement (Application::ShutDown), and 30 seconds of headroom. Until
// 10 October 2026 it was the 30 seconds alone, and an update, the silent
// installer a live update runs, or an uninstall refused with "did not stop"
// while the service still waited, within its minute, for an indexer in a long
// statement.
function GetServiceStopBudgetMs() : Integer;
var
   szIniFile   : AnsiString;
   drainSeconds: Integer;
begin
   drainSeconds := 0;
   szIniFile := GetInifile();
   if (Length(szIniFile) > 0) then
      drainSeconds := GetIniInt('Settings', 'ShutdownDrainSeconds', 0, 0, 3600, szIniFile);

   Result := (drainSeconds + 90) * 1000;
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
		       'this setup again.', mbError, MB_OK, IDOK);
		Result := false;
	end;
end;

// The mark a new installation leaves when its database step does not complete, so
// that what follows that step is still done when the database is set up later:
// HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer (the 32-bit view, beside InstallLocation),
// NewInstallationPending holding the account the service was to run as - virtual or
// localsystem - and NewInstallationProfile the profile chosen, if any. Running this
// setup again finishes such an installation as a new one (IsNewService,
// IsNewDatabase), and so does DBSetup.exe, run by an administrator, which reads
// the mark (DBSetup's NewInstallation.cs) and runs the steps the installer runs
// after a database step: the secure defaults, the profile, the stored-secrets key,
// NT SERVICE\hMailServer's login on SQL Server with Windows authentication, and the
// move to that account. The installer clears the mark before its own database step,
// so that the DBSetup it runs leaves those steps to it, and writes it again only if
// that step does not complete; the uninstaller removes it (CurUninstallStepChanged).
// Until 6.3.6 such an installation, set up again either way, kept LocalSystem, and
// one DBSetup set up had no self-signed certificate and no stored-secrets key (the
// installer smoke VM, 2 October 2026).
const
   PENDING_KEY = 'SOFTWARE\hMailServer';
   PENDING_ACCOUNT_VALUE = 'NewInstallationPending';
   PENDING_PROFILE_VALUE = 'NewInstallationProfile';

// Whether an earlier run of this setup left the mark: read once, before this
// installation clears it, with the account and the profile it holds
// (g_szPendingAccount, g_szPendingProfile), which this installation takes for
// its own unless /serviceaccount=, /profile= or the pages choose otherwise.
//
// Not for an update hMailServer.Updater runs (g_szUpgradeToken: the live update,
// or Install update in the Control Panel): that is an unattended upgrade of a
// server in service, which its database step can only reach by running, and which
// the Updater puts back on exit code 10 and leaves on 11. Taken for a new
// installation's, it would be moved to NT SERVICE\hMailServer by default, given a
// new installation's defaults, and its database step would not be held to the
// version before it (BeginDatabaseStep), so that a step which changed the
// database could end in 10 and be put back. The mark is left for an administrator
// running this setup, or DBSetup, to finish.
function PendingNewInstallation(): Boolean;
var
   szAccount: String;
   szProfile: String;
begin
   if not g_bPendingKnown then
   begin
      g_bPendingBefore := RegQueryStringValue(HKLM32, PENDING_KEY, PENDING_ACCOUNT_VALUE, szAccount) and (Trim(szAccount) <> '');
      g_bPendingKnown := true;
      if g_bPendingBefore and (g_szUpgradeToken <> '') then
      begin
         g_bPendingBefore := false;
         Log('An earlier run of setup left a new installation unfinished (' + PENDING_ACCOUNT_VALUE + ' = ' + szAccount + '). This is an update run by hMailServer.Updater, which upgrades the installation as it is and leaves the mark for an administrator: this setup run by hand, or DBSetup.exe, finishes it.');
      end
      else if g_bPendingBefore then
      begin
         g_szPendingAccount := Lowercase(Trim(szAccount));
         if RegQueryStringValue(HKLM32, PENDING_KEY, PENDING_PROFILE_VALUE, szProfile) then
            g_szPendingProfile := Lowercase(Trim(szProfile));
         Log('An earlier run of this setup left a new installation unfinished (' + PENDING_ACCOUNT_VALUE + ' = ' + szAccount + '): its database step did not complete. This installation finishes it as a new one.');
      end;
   end;
   Result := g_bPendingBefore;
end;

// The account an unfinished new installation chose (PendingNewInstallation), as
// the default for this one, which finishes it: LocalSystem when that one was to
// stay on it - localsystem, or keep, which on a service registered for the
// database step is LocalSystem - and otherwise the virtual account, as for any
// new installation. DBSetup.exe, finishing it instead, moves the service only
// for virtual (NewInstallation.cs), so either way it ends on the same account.
function PendingAccountDefault(): String;
begin
   if PendingNewInstallation() and ((g_szPendingAccount = 'localsystem') or (g_szPendingAccount = 'keep')) then
      Result := 'localsystem'
   else
      Result := 'virtual';
end;

procedure ClearPendingNewInstallation();
begin
   RegDeleteValue(HKLM32, PENDING_KEY, PENDING_ACCOUNT_VALUE);
   RegDeleteValue(HKLM32, PENDING_KEY, PENDING_PROFILE_VALUE);
end;

procedure RecordPendingNewInstallation(szAccount: String; szProfile: String);
begin
   if RegWriteStringValue(HKLM32, PENDING_KEY, PENDING_ACCOUNT_VALUE, szAccount) then
   begin
      if szProfile <> '' then
         RegWriteStringValue(HKLM32, PENDING_KEY, PENDING_PROFILE_VALUE, szProfile)
      else
         RegDeleteValue(HKLM32, PENDING_KEY, PENDING_PROFILE_VALUE);
      Log('The new installation is not finished: ' + PENDING_ACCOUNT_VALUE + ' = ' + szAccount + ' is left for this setup run again, or DBSetup.exe run by an administrator, to finish it.');
   end
   else
      Log('The mark of an unfinished new installation could not be written; this setup run again still finishes it as a new one while hMailServer.INI names no database.');
end;

// Whether this installation makes a new database: asked once, of the settings file
// as it was before the installation touched it - an upgrade has a database type
// there, a new installation has none - and of the mark an unfinished new
// installation leaves (PendingNewInstallation): a database DBSetup set up after an
// earlier run of this setup failed its database step belongs to a new installation
// still, which this one finishes. A profile is offered to a new server only:
// applied to a server already in service, it could close a listener or turn a
// filter off that somebody depends on, and the Control Deck and the Control Panel
// show that server the diff first.
function IsNewDatabase(): Boolean;
begin
   if not g_bNewDatabaseKnown then
   begin
      g_bNewDatabase := (GetCurrentDatabaseType() = '') or PendingNewInstallation();
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
      // /profile=, or else the profile an unfinished new installation chose,
      // which this one finishes (PendingNewInstallation).
      szProfile := Lowercase(Trim(ExpandConstant('{param:profile|}')));
      if (szProfile = '') and PendingNewInstallation() then
         szProfile := g_szPendingProfile;
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

// Runs hMailServer.exe with szArguments, what it says on its standard output and
// standard error going into a file in {tmp}, and from there into the setup log
// line for line and into szSaid. An administrator's verb writes nothing to the
// error log or the Application log - the log folder is the service's to write
// (hMailServer.cpp, ForbidServiceFolderWrites) - so what it did, and why it
// failed, is said there and nowhere else. False when it could not be run;
// ResultCode is its exit code otherwise.
function RunServerVerbReported(szArguments: String; var ResultCode: Integer; var szSaid: String): Boolean;
var
   szReport: String;
   szText: AnsiString;
begin
   szSaid := '';
   szReport := ExpandConstant('{tmp}\server-verb-output.txt');
   DeleteFile(szReport);
   Result := Exec(ExpandConstant('{cmd}'), '/s /c ""' + ExpandConstant('{app}\Bin\hMailServer.exe') + '" ' + szArguments + ' > "' + szReport + '" 2>&1"',
                  '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
   if not Result then
   begin
      Log('hMailServer.exe ' + szArguments + ' could not be run: ' + SysErrorMessage(ResultCode));
      Exit;
   end;

   if LoadStringFromFile(szReport, szText) then
      szSaid := Trim(String(szText));
   DeleteFile(szReport);

   if szSaid <> '' then
      Log('hMailServer.exe ' + szArguments + ' returned ' + IntToStr(ResultCode) + ':' + #13#10 + szSaid)
   else
      Log('hMailServer.exe ' + szArguments + ' returned ' + IntToStr(ResultCode) + ' and said nothing.');
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
   szSaid: String;
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

   if (RunServerVerbReported('--apply-profile ' + szProfile + ' --once --if-new --confirm', ResultCode, szSaid) = False) then
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
      szWhy := 'hMailServer.exe --apply-profile returned ' + IntToStr(ResultCode) + '. It said: ' + szSaid;
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
   szSaid: String;
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

   if (RunServerVerbReported('--create-stored-secrets-key', ResultCode, szSaid) = False) then
   begin
      SuppressibleMsgBox('The stored-secrets key was not made: hMailServer.exe did not start. ' + SysErrorMessage(ResultCode) + #13#10#13#10 +
             'hMailServer is installed, and stores each account''s second factor and the secret settings as written until a key is made. From an elevated prompt in the hMailServer Bin folder: hMailServer.exe --create-stored-secrets-key, then hMailServer.exe --export-stored-secrets-key FILE.', mbInformation, MB_OK, IDOK);
      Exit;
   end;

   // The account running setup could not open the database, which the service
   // can: the service makes the key at the start this installation makes
   // (g_bFinishNewInstallationAtStart).
   if (ResultCode <> 0) and (Pos('The database could not be opened', szSaid) > 0) then
   begin
      g_bFinishNewInstallationAtStart := true;
      SuppressibleMsgBox('The stored-secrets key could not be made by the account running setup, which cannot open the database:' + #13#10#13#10 + szSaid + #13#10#13#10 +
             'The hMailServer service, which can, makes it at the start this setup makes ("sc start hMailServer --apply-new-install-defaults"), when it runs as LocalSystem; the Control Deck''s setup checklist shows whether it holds one, and the service reports HM7498 if it could not make it. ' +
             'Then export it from an elevated prompt in the hMailServer Bin folder: hMailServer.exe --export-stored-secrets-key FILE.', mbInformation, MB_OK, IDOK);
      Exit;
   end;

   if (ResultCode <> 0) then
   begin
      SuppressibleMsgBox('The stored-secrets key was not made (hMailServer.exe --create-stored-secrets-key returned ' + IntToStr(ResultCode) + '). It said:' + #13#10#13#10 + szSaid + #13#10#13#10 +
             'hMailServer is installed. Where another server holds the database''s key, import it here with hMailServer.exe --import-stored-secrets-key FILE, from an elevated prompt in the hMailServer Bin folder.', mbInformation, MB_OK, IDOK);
      Exit;
   end;

   // Found, not made: a key this server already held, the database's - setup run
   // again to finish an installation whose key an earlier run made. Until 6.3.6
   // this said it had made one.
   if (Pos('already holds the stored-secrets key', szSaid) > 0) then
   begin
      SuppressibleMsgBox('hMailServer already holds this installation''s stored-secrets key, made earlier, which the database records; no key was made now.' + #13#10#13#10 +
             'If it has not been exported yet, export it and keep the file with your database backups, stored apart from them. From an elevated prompt in the hMailServer Bin folder:' + #13#10#13#10 +
             'hMailServer.exe --export-stored-secrets-key FILE', mbInformation, MB_OK, IDOK);
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
   szSaid: String;
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

   if (RunServerVerbReported('--apply-new-install-defaults', ResultCode, szSaid) = False) then
   begin
      SayNewInstallDefaultsNotApplied('hMailServer.exe did not start. ' + SysErrorMessage(ResultCode), true);
      Exit;
   end;

   case ResultCode of
      0: Exit;
      5:
         begin
            if bBuiltIn then
               SayNewInstallDefaultsNotApplied('the built-in database DBSetupQuick has just created was not recognised as a new one (exit code 5), which should not happen; its listeners were left as the create step made them.', false);
         end;
      2:
         begin
            // The database could not be opened by the account running setup - on
            // SQL Server with Windows authentication, one that is no SQL Server
            // administrator - or not read well enough to judge: the mark is kept,
            // and the service, which opens it, applies them at the start this
            // setup makes (g_bFinishNewInstallationAtStart).
            g_bFinishNewInstallationAtStart := true;
            Log('The secure defaults are left to the service''s start: sc start hMailServer --apply-new-install-defaults.');
         end;
      4: SayNewInstallDefaultsNotApplied('the hMailServer service was running again when the defaults were to be applied (exit code 4).', true);
   else
      SayNewInstallDefaultsNotApplied('hMailServer.exe --apply-new-install-defaults returned ' + IntToStr(ResultCode) + '. It said what was and was not changed:' + #13#10#13#10 + szSaid + #13#10#13#10 + 'Where it says nothing was changed (the certificate could not be made, written or recorded), the database keeps its mark, and running it again from the Bin folder with the service stopped still applies them.', false);
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

function IsLocalSystemAccount(Account: String): Boolean;
begin
   Result := (CompareText(Account, 'LocalSystem') = 0) or (CompareText(Account, '.\LocalSystem') = 0) or
             (CompareText(Account, 'NT AUTHORITY\SYSTEM') = 0);
end;

// The folder of the installation found before this one, read without {app}, which
// is not known yet when the wizard's pages are made: the InstallLocation every
// installation writes under HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer in the 32-bit
// view (section_registry.iss), else the uninstall entry's - in the 64-bit view,
// where a 64-bit installation's entry is, or the 32-bit view of an x86 one. ''
// when there is none.
function PreviousInstallationFolder(): String;
var
   szFolder: String;
begin
   Result := '';
   if RegQueryStringValue(HKLM32, 'SOFTWARE\hMailServer', 'InstallLocation', szFolder) and (Trim(szFolder) <> '') then
      Result := Trim(szFolder)
   else if IsWin64 and RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1', 'InstallLocation', szFolder) and (Trim(szFolder) <> '') then
      Result := Trim(szFolder)
   else if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\hMailServer_is1', 'InstallLocation', szFolder) and (Trim(szFolder) <> '') then
      Result := Trim(szFolder);
end;

// Whether the installation found before this one has a settings file, and it names
// no database: the file in that installation's folder (PreviousInstallationFolder),
// or, for a very old installation, the one in the Windows folder. False when no
// settings file is found: an installation whose file cannot be read is not taken
// for a new one whose database step did not complete, which would move its
// LocalSystem service to the virtual account by default. Inno Setup writes a 64-bit
// installation's uninstall entry in the 64-bit view, so a look in the 32-bit view
// alone finds no folder there, and took every LocalSystem service upgraded for such
// a one (the installer smoke VM, 3 October 2026, before this was committed).
function NoDatabaseConfiguredBefore(): Boolean;
var
   szFolder: String;
   szFile: String;
begin
   szFile := '';
   szFolder := PreviousInstallationFolder();
   if (szFolder <> '') and FileExists(AddBackslash(szFolder) + 'Bin\hMailServer.ini') then
      szFile := AddBackslash(szFolder) + 'Bin\hMailServer.ini'
   else if FileExists(ExpandConstant('{win}\hMailServer.ini')) then
      szFile := ExpandConstant('{win}\hMailServer.ini');

   Result := (szFile <> '') and (Trim(GetIniString('Database', 'Type', '', szFile)) = '');
   if szFile = '' then
      Log('No settings file of an earlier installation was found (its folder: "' + szFolder + '").')
   else if Result then
      Log('The earlier installation''s settings file, ' + szFile + ', names no database.');
end;

// Whether this installation sets the service up as a new one: there is no
// service yet, or the one there is was left by a new installation whose database
// step did not complete - the mark it leaves (PendingNewInstallation), or, from a
// setup that left none, a service registered as LocalSystem, which is how a new
// installation registers it for its database step whatever account it chose,
// beside a settings file that names no database. Such a service runs as
// LocalSystem because the step failed, not because anybody kept it so, and it is
// offered, and given by default, the account a new installation gets:
// /serviceaccount= and the page choose, as for any new installation.
//
// Asked once, as the account before is, before the database step writes
// [Database] into the settings file.
function IsNewService(): Boolean;
begin
   if not g_bNewServiceKnown then
   begin
      g_bNewService := (GetServiceAccountBefore() = '') or PendingNewInstallation() or
                       (IsLocalSystemAccount(GetServiceAccountBefore()) and NoDatabaseConfiguredBefore());
      g_bNewServiceKnown := true;
      if g_bNewService and (GetServiceAccountBefore() <> '') then
         Log('The hMailServer service there runs as ' + GetServiceAccountBefore() + ', which a new installation whose database step did not complete leaves: it is set up as a new installation''s.');
   end;
   Result := g_bNewService;
end;

// The account chosen for the service: virtual (NT SERVICE\hMailServer),
// localsystem or keep. On the page, or with /serviceaccount= for a silent
// installation, which is itself the choice; otherwise what the page offers first:
// the virtual account for a new installation - or the account an unfinished one
// chose, which this one finishes (PendingAccountDefault) - the account the
// service has for an upgrade. Anything else given with /serviceaccount= is
// ignored, and said in the setup log.
function GetServiceAccountChoice(): String;
var
   szChoice: String;
begin
   if IsNewService() then
      Result := PendingAccountDefault()
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

   // A new installation's page offers the virtual account first, whatever it
   // preselects (CreateWizardPages): its answer is the choice either way.
   if IsNewService() then
   begin
      if (g_pageServiceAccount.SelectedValueIndex = 1) then
         Result := 'localsystem'
      else
         Result := 'virtual';
   end
   else if (g_pageServiceAccount.SelectedValueIndex = 1) then
   begin
      if IsLocalSystemAccount(GetServiceAccountBefore()) then
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

// Whether hMailServer.INI names SQL Server reached with Windows authentication -
// no user name - on which the service signs in as the account it runs as.
function DatabaseIsMssqlIntegrated(): Boolean;
var
   szIniFile: String;
begin
   Result := false;
   szIniFile := GetInifile();
   if szIniFile = '' then
      Exit;
   Result := (CompareText(GetIniString('Database', 'Type', '', szIniFile), 'MSSQL') = 0) and
             (Trim(GetIniString('Database', 'Username', '', szIniFile)) = '');
end;

// The sentence the move page adds when the database is SQL Server reached with
// Windows authentication: the moved service signs in to it as
// NT SERVICE\hMailServer, which has no login there until one is made - which setup
// makes before the move, as the administrator running it, where that account may
// (GrantServiceDatabaseLogin).
function MssqlIntegratedSentence(): String;
begin
   Result := '';
   if DatabaseIsMssqlIntegrated() then
      Result := ' The database is SQL Server with Windows authentication: before the move, setup gives NT SERVICE\hMailServer a login there, a user in the hMailServer database and db_owner, as your account. ' +
                'If your account may not, the service stays on LocalSystem, and setup says what a SQL Server administrator runs.';
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
   // The profile an unfinished new installation chose, which this one finishes
   // (PendingNewInstallation): offered again as the answer it gave.
   if PendingNewInstallation() then
   begin
      if g_szPendingProfile = 'office' then g_pageProfile.SelectedValueIndex := 1
      else if g_szPendingProfile = 'gateway' then g_pageProfile.SelectedValueIndex := 2
      else if g_szPendingProfile = 'relay' then g_pageProfile.SelectedValueIndex := 3
      else if g_szPendingProfile = 'sender' then g_pageProfile.SelectedValueIndex := 4;
   end;

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
   // An unfinished new installation that chose LocalSystem: offered again as it
   // chose (PendingAccountDefault).
   if IsNewService() and (PendingAccountDefault() = 'localsystem') then
      g_pageServiceAccount.SelectedValueIndex := 1;

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
   g_szLastDatabaseReadError := '';
   try
      App := CreateOleObject('hMailServer.Application');
      Database := App.Database;
      Result := Database.CurrentVersion;
      if Result > 0 then
         Log('The database is at version ' + IntToStr(Result) + ' ' + szWhen + ' (Database.CurrentVersion, through the hMailServer service).')
      else
      begin
         g_szLastDatabaseReadError := 'the hMailServer service answered ' + IntToStr(Result);
         Log('The database''s version could not be read ' + szWhen + ': ' + g_szLastDatabaseReadError + '.');
         Result := -1;
      end;
   except
      Result := -1;
      g_szLastDatabaseReadError := Trim(GetExceptionMessage);
      Log('The database''s version could not be read ' + szWhen + ': ' + g_szLastDatabaseReadError);
   end;
   Database := Unassigned;
   App := Unassigned;
end;

#include "OriginalHMailServer.iss"

// Whether the installation this setup would replace is one of the original
// hMailServer's 5.7 whose database this fork cannot upgrade (OriginalHMailServer.iss
// says what identifies one and why it is refused): the refusal's words, or '' when
// it is not. Asked first in InitializeSetup, before anything is installed, stopped
// or changed. Nothing is asked of a server whose hMailServer.exe is not 5.7 or later
// before 6 - every installation of this fork's, and the original's up to 5.6 - and
// a stopped service is not started for it: COM activation would start it.
function OriginalHMailServerRefusal(): String;
var
   szFolder: String;
   szExe: String;
   szServerVersion: String;
   wMajor, wMinor, wRevision, wBuild: Word;
   iMajor, iMinor: Integer;
   iDatabase: Integer;
   bScripts: Boolean;
   iKind: Integer;
   FindRec: TFindRec;
begin
   Result := '';
   szFolder := PreviousInstallationFolder();
   if szFolder = '' then
      Exit;
   szExe := AddBackslash(szFolder) + 'Bin\hMailServer.exe';
   iMajor := -1;
   iMinor := -1;
   if FileExists(szExe) and GetVersionComponents(szExe, wMajor, wMinor, wRevision, wBuild) then
   begin
      iMajor := wMajor;
      iMinor := wMinor;
      szServerVersion := IntToStr(wMajor) + '.' + IntToStr(wMinor) + '.' + IntToStr(wRevision) + ' build ' + IntToStr(wBuild);
   end;
   if not IsFiveSevenServer(iMajor, iMinor) then
      Exit;

   Log('The installation in ' + szFolder + ' has hMailServer.exe ' + szServerVersion + ', which may be the original hMailServer''s 5.7: its database''s version decides.');
   iDatabase := -1;
   if IsServiceRunning('hMailServer') then
      iDatabase := ReadDatabaseVersion('before this installation, to tell the original''s 5.7 from a database this build upgrades')
   else
      Log('The hMailServer service is not running, so the database''s version is not read; the DBScripts folder decides.');

   bScripts := False;
   if iDatabase <= 0 then
   begin
      if FindFirst(AddBackslash(szFolder) + 'DBScripts\Upgrade' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) + 'to*.sql', FindRec) then
      begin
         try
            repeat
               if IsOriginalOnlyScript(FindRec.Name) then
               begin
                  bScripts := True;
                  Log('The DBScripts folder holds ' + FindRec.Name + ', a step of the original''s 5.7 past version ' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) + '.');
               end;
            until bScripts or (FindNext(FindRec) = False);
         finally
            FindClose(FindRec);
         end;
      end;
   end;

   iKind := OriginalHMailServerKind(iMajor, iMinor, iDatabase, bScripts);
   Result := OriginalHMailServerMessage(iKind, szServerVersion, iDatabase, szFolder);
   if Result = '' then
      Log('Not refused as the original''s 5.7: neither the database''s version nor, with that unread, the DBScripts folder shows a version past ' +
          IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) + '.');
end;

function InitializeSetup(): Boolean;
	var
		sMessage : AnsiString;
		szRefusal : String;
		SoftwareVersion: AnsiString;
begin
	Result := true;

	// An installation of the original hMailServer's 5.7, whose database this build
	// cannot upgrade, is refused before anything else: nothing has been installed,
	// stopped or changed yet (OriginalHMailServerRefusal). Setup then ends with
	// exit code 1, Inno Setup's for a setup that did not initialise, and the
	// refusal is in its log.
	szRefusal := OriginalHMailServerRefusal();
	if szRefusal <> '' then
	begin
		Log('Refused: ' + szRefusal);
		SuppressibleMsgBox(szRefusal, mbCriticalError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

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

	// The two checks for a program that is open are for an installation somebody
	// started by hand. An administrator's install (g_bAdministratorUpdate) comes
	// from hMailServer.Updater, which has ended every program running from the
	// installation before it ran this, the Control Panel that asked included; a
	// Control Panel anywhere else - a stray copy, or any program that names a
	// window so - would otherwise stop the install it asked for.
	//
	// The Control Panel is found by the mutex it holds while it runs
	// (Services\ControlPanelRunningMark.cs, the same name) as well as by its
	// window's title: signed in, the title names the server it manages first,
	// "mail.example.com - hMailServer Control Panel", and in Japanese, Swedish,
	// Turkish and Chinese the product's name in it is translated, so the title
	// alone missed a Control Panel that was open. The title still finds one from
	// before the mutex.
	if (not g_bAdministratorUpdate) and (CheckForMutexes('hMailServerControlPanel') or (FindWindowByWindowName('hMailServer Control Panel') > 0)) then
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
		szSaid: String;
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
	if (RunServerVerbReported(szParameters, ResultCode, szSaid) = False) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;

	if (ResultCode <> 0) then
	begin
		SuppressibleMsgBox('The hMailServer service could not be created (hMailServer.exe /Register returned ' + IntToStr(ResultCode) + ').' + #13#10#13#10 +
		       szSaid + #13#10#13#10 +
		       'Run "hMailServer.exe --service-account ' + szAccountChoice + ' /Register" as an administrator from the ' +
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

// Starts the hMailServer service with the argument that has it finish a new
// installation the account running setup could not (g_bFinishNewInstallationAtStart):
// "sc start hMailServer --apply-new-install-defaults" - the secure defaults applied
// and the stored-secrets key made by the service, which opens the database, before
// its listeners start (FinishNewInstallationAtStart in hMailServer.cpp). sc.exe
// returns once the start is asked for, so the service is waited for, as net start
// waits. False when sc.exe could not be run or the start was refused.
function StartServiceToFinishNewInstallation(var ResultCode: Integer): Boolean;
var
   I: Integer;
begin
   Result := Exec(ExpandConstant('{sys}\sc.exe'), 'start hMailServer --apply-new-install-defaults', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
   if not Result then
      Exit;
   if ResultCode <> 0 then
   begin
      Log('sc.exe start hMailServer --apply-new-install-defaults returned ' + IntToStr(ResultCode) + '.');
      Result := false;
      Exit;
   end;
   Log('Started the hMailServer service to apply a new installation''s defaults and make its stored-secrets key (--apply-new-install-defaults); the service reports what it could not do (HM7498, HM7510, HM7511), and its application log, when that is on, each step.');
   for I := 1 to 60 do
   begin
      if IsServiceRunning('hMailServer') then
         Exit;
      Sleep(1000);
   end;
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

// Runs hMailServer.exe /ProtectFolders (Common/Util/ServiceAccount.h) at every
// installation and upgrade of the server, whatever account the service runs as:
// the program folder's own Data, Logs, Temp, Database and Events folders and
// Bin\hMailServer.INI made private to SYSTEM, the Administrators and the service;
// and the program folder itself, Bin and ControlPanel hardened - SYSTEM and the
// Administrators the only ones who may change them, BUILTIN\Users Read & execute.
// Until 6.3.6 the six inherited Read for BUILTIN\Users from Program Files, so every
// user of the computer could read the mail, the logs, the built-in database and
// the database's password; until 6.3.7 an installation at a drive's root left
// every user free to replace the programs in Bin, which the service and the
// administrators run. Run before the database step, so that the database it
// makes and the logs the service starts are made inside folders already private.
// What the verb said goes into the setup log, line for line. One that could not
// make each of them private, or harden each, says so, and the installation goes
// on: what it could not change is as it was, and the server runs as it did.
procedure ProtectFolders();
	var
		ResultCode: Integer;
		szReport: String;
		szSaid: AnsiString;
begin
	szReport := ExpandConstant('{tmp}\protect-folders.txt');
	DeleteFile(szReport);

	if (Exec(ExpandConstant('{cmd}'), '/s /c ""' + ExpandConstant('{app}\Bin\hMailServer.exe') + '" /ProtectFolders > "' + szReport + '" 2>&1"',
	         '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = False) then
	begin
		Log('hMailServer.exe /ProtectFolders could not be run: ' + SysErrorMessage(ResultCode));
		ResultCode := -1;
	end
	else if LoadStringFromFile(szReport, szSaid) then
		Log('hMailServer.exe /ProtectFolders returned ' + IntToStr(ResultCode) + ':' + #13#10 + String(szSaid))
	else
		Log('hMailServer.exe /ProtectFolders returned ' + IntToStr(ResultCode) + ' and wrote nothing to ' + szReport + '.');

	if (ResultCode <> 0) then
		SuppressibleMsgBox('The data, log, temp, database and event folders of hMailServer and hMailServer.INI could not all be made private to the administrators and the service, or the program folder, Bin and ControlPanel made theirs alone to change: hMailServer.exe /ProtectFolders returned ' + IntToStr(ResultCode) + ', and the setup log says why. ' +
		       'What it could not change is as it was: readable by every user of this computer, or for a program folder at a drive''s root, changeable by every user. Run "hMailServer.exe /ProtectFolders" as an administrator from the hMailServer Bin folder to see what it says and to retry.', mbError, MB_OK, IDOK);
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
		7: Result := 'the account running setup could not read the database, so the certificate and key files and the backup folder it names are not known, and a silent installation does not move the service without them - run "hMailServer.exe --confirm /GrantServiceAccess" and answer Yes, then "hMailServer.exe --service-account virtual /Register", or move it from the Control Panel, which asks';
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

	// 7: the database could not be read by this account; the service already
	// runs as it, and nothing is moved here.
	if (ResultCode = 0) or (ResultCode = 4) or (ResultCode = 7) then
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

// hMailServer.exe --grant-database-login (ServiceAccount::RunDatabaseLoginCommand),
// run as this installer - the administrator who runs it, elevated - over a
// connection of its own: what NT SERVICE\hMailServer needs to open a SQL Server
// database reached with Windows authentication on this computer, a login, a user
// in the database and db_owner. Run through cmd.exe to capture its lines, as
// /GrantServiceAccess is; they go into the setup log, and back in szSaid for a
// message - what it made, or what it could not and the statements a SQL Server
// administrator runs instead. False when it could not be run; ResultCode is its
// exit code otherwise: 0 the account can open the database, or needs nothing (not
// SQL Server with Windows authentication, or a SQL Server on another computer,
// where the virtual account is this computer's account, as LocalSystem is), 1
// and 2 it cannot.
function RunDatabaseLoginVerb(var ResultCode: Integer; var szSaid: String): Boolean;
var
	szReport: String;
	szLines: AnsiString;
begin
	szSaid := '';
	szReport := ExpandConstant('{tmp}\grant-database-login.txt');
	DeleteFile(szReport);

	Result := Exec(ExpandConstant('{cmd}'), '/s /c ""' + ExpandConstant('{app}\Bin\hMailServer.exe') + '" --grant-database-login > "' + szReport + '" 2>&1"',
	               '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
	if not Result then
	begin
		szSaid := 'hMailServer.exe --grant-database-login could not be run: ' + SysErrorMessage(ResultCode);
		Log(szSaid);
		Exit;
	end;

	if LoadStringFromFile(szReport, szLines) then
		szSaid := Trim(String(szLines));
	Log('hMailServer.exe --grant-database-login returned ' + IntToStr(ResultCode) + ':' + #13#10 + szSaid);
end;

// Before the move to NT SERVICE\hMailServer: when the database is SQL Server
// reached with Windows authentication, that account is given what it needs to
// open it (RunDatabaseLoginVerb) - by the account that runs this setup, while the
// service is still LocalSystem's, and before the service is registered as the
// virtual account. The database step made the database, or opened it, as
// LocalSystem, and the moved service signs in as NT SERVICE\hMailServer: until
// 6.3.6 setup moved it without a login there, the service could not open its
// database (HM5028, HM4354), nothing listened, and setup ended with 0 (the
// installer smoke VM, 2 October 2026). True when the move may go ahead: the
// account can open the database, or needs nothing - or the verb could not give it
// what it needs, and the database is one whose login and user a SQL Server
// administrator may have made already: attached, chosen in DBSetup's wizard, or
// there before this installation (the statements this says, run after an earlier
// installation could not). The move is then made, and held to the service's
// opening its database (CheckServiceAfterMove), which takes it back to LocalSystem
// when it does not. False, said with what the verb said - the statements a SQL
// Server administrator runs among it - when it could not on a database this
// installation's DBSetup has just created (g_bDatabaseCreatedHere), which no login
// of that account can have reached yet: the service then stays on LocalSystem,
// working, without a start that could only fail, and the installation fails
// (g_bDatabaseLoginFailed) rather than end with 0 on a server it knows will not
// open its database.
function GrantServiceDatabaseLogin(): Boolean;
var
	ResultCode: Integer;
	szSaid: String;
begin
	Result := true;
	g_szDatabaseLoginSaid := '';
	if (DatabaseIsMssqlIntegrated() = false) then
		Exit;

	if RunDatabaseLoginVerb(ResultCode, szSaid) and (ResultCode = 0) then
		Exit;

	g_szDatabaseLoginSaid := szSaid;
	if (g_bDatabaseCreatedHere = false) then
	begin
		Log('NT SERVICE\hMailServer could not be given its login on SQL Server by this account, and the database was not created by this installation, so a SQL Server administrator may have given it one: the move is made, and held to the service''s opening its database.');
		Exit;
	end;

	Result := false;
	g_bDatabaseLoginFailed := true;
	SuppressibleMsgBox('The hMailServer service was not moved to NT SERVICE\hMailServer, and runs as LocalSystem. The database is SQL Server with Windows authentication, ' +
	       'and that account could not be given what it needs to open it - a login, a user in the database and db_owner - so moved, the service would not open its database and would serve no mail.' + #13#10#13#10 +
	       szSaid + #13#10#13#10 +
	       'Once a SQL Server administrator has run those statements, move the service from the Control Panel (Windows service account), or run this setup again with the move chosen - attended, since this account cannot read the database and only an administrator''s Yes moves it then. The service applies the secure defaults and makes the stored-secrets key itself at the start this setup makes.', mbError, MB_OK, IDOK);
end;

// The move to NT SERVICE\hMailServer, after the database step
// (MoveServiceAfterDatabase): that account's login on SQL Server with Windows
// authentication (GrantServiceDatabaseLogin), the service stopped, its folders
// granted - with the question the verb asks when the configuration names anything
// outside them - and only then registered as the virtual account. A grant short
// of a clean one leaves the service on LocalSystem, working, and says why; the
// installation still succeeds. A login on SQL Server that could not be given
// leaves it there too, and fails the installation (g_bDatabaseLoginFailed). True
// when the service was moved, which CheckServiceAfterMove then holds to its
// opening its database.
function MoveServiceToVirtualAccount(): Boolean;
	var
		ResultCode: Integer;
		szWhy: String;
begin
	Result := false;

	if (GrantServiceDatabaseLogin() = false) then
		Exit;

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
	Result := true;
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

// Whether hMailServer.INI sets [Database] UpgradeOnStart, on which the service's own
// start runs the schema steps (Application::OnDatabaseConnected): any value but none,
// empty or 0 is taken as set, so that a value the service might read as 1 is never
// taken for off.
function UpgradeOnStartSet(): Boolean;
var
   szValue: String;
begin
   szValue := Trim(GetIniString('Database', 'UpgradeOnStart', '', GetInifile()));
   Result := (szValue <> '') and (szValue <> '0');
end;

// What DBSetupQuick.exe said about a failure, from the report it was told to write
// (report:PATH; a Windows program started here has no output to read): into this
// installation's log, its lines after the first unstamped, as the grant's report
// is, and back for a message. Empty when it wrote nothing.
function ReadDBSetupQuickReport(szReport: String; ResultCode: Integer): String;
var
   szLines: AnsiString;
begin
   Result := '';
   if LoadStringFromFile(szReport, szLines) then
      Result := Trim(String(szLines));
   if Result <> '' then
      Log('DBSetupQuick.exe returned ' + IntToStr(ResultCode) + ':' + #13#10 + Result)
   else if ResultCode <> 0 then
      Log('DBSetupQuick.exe returned ' + IntToStr(ResultCode) + ' and wrote nothing to ' + szReport + '.');
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
// any other failed step is. So is one whose step could not run at all
// (bStepRanNothing): DBSetupQuick could not reach the service through COM (exit
// code 4), so neither it nor DBUpdater ran, and the one other thing that moves a
// database here, the service's own start with [Database] UpgradeOnStart, is off
// (UpgradeOnStartSet). Otherwise a version that cannot be read leaves the
// database "may have changed", and nothing is put back.
procedure EndDatabaseStep(bStepRanNothing: Boolean);
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
   else if bStepRanNothing and (g_iDatabaseVersionAfter <= 0) and (UpgradeOnStartSet() = false) then
   begin
      g_bDatabaseChanged := false;
      if g_iDatabaseVersionBefore > 0 then
         Log('Database step left the database unchanged, at version ' + IntToStr(g_iDatabaseVersionBefore) + ': DBSetupQuick.exe could not reach the hMailServer service (exit code 4), ' +
             'so neither it nor DBUpdater ran, and [Database] UpgradeOnStart is not set, so the service''s own start ran no schema step, though its version after the step could not be read.')
      else
         Log('Database step left the database unchanged: DBSetupQuick.exe could not reach the hMailServer service (exit code 4), so neither it nor DBUpdater ran, ' +
             'and [Database] UpgradeOnStart is not set, so the service''s own start ran no schema step, though its version could not be read before the step or after it.');
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

// After the start of a service this installation moved to NT SERVICE\hMailServer:
// whether it opened its database as that account - its version, read through the
// service (ReadDatabaseVersion), is the one this build needs. The database step ran
// as LocalSystem; the moved service is the first thing to open the database as the
// virtual account, and a service reports itself running before it has opened
// anything, so a running service proves nothing. One that did not start, or did
// not open its database - a SQL Server login that GrantServiceDatabaseLogin could
// not see was missing, say - is stopped, registered as LocalSystem again, as it was
// for the database step, and started, and the installation fails in words of its
// own (g_bDatabaseLoginFailed): until 6.3.6 it ended with 0 on a server that served
// no mail. True when the moved service runs on its database.
function CheckServiceAfterMove(): Boolean;
var
   ResultCode: Integer;
   iVersion: Integer;
   szWhat: String;
   szNow: String;
begin
   Result := true;
   if IsServiceRunning('hMailServer') then
   begin
      iVersion := ReadDatabaseVersion('after the move, through the service running as NT SERVICE\hMailServer');
      if iVersion = {#HM_REQUIRED_DB_VERSION} then
         Exit;
      if iVersion > 0 then
         szWhat := 'it opened its database at version ' + IntToStr(iVersion) + ', where this build needs {#HM_REQUIRED_DB_VERSION}'
      else
         szWhat := 'it started, and did not open its database (' + g_szLastDatabaseReadError + ')';
   end
   else
      szWhat := 'it did not start';

   Result := false;
   g_bDatabaseLoginFailed := true;
   Log('The hMailServer service, moved to NT SERVICE\hMailServer: ' + szWhat + '. It is moved back to LocalSystem.');

   if StopHMailServerAndWaitForExit(90000) and RegisterHMailServerService('localsystem') then
   begin
      if (Exec(ExpandConstant('{sys}\net.exe'), 'START hMailServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) = true) and IsServiceRunning('hMailServer') then
         szNow := 'It was moved back to LocalSystem, where it runs.'
      else
         szNow := 'It was moved back to LocalSystem, and did not start there either: the hMailServer error log says why.';
   end
   else
      szNow := 'It could not be moved back to LocalSystem: run "hMailServer.exe --service-account localsystem /Register" from an elevated prompt in the hMailServer Bin folder, then start the service.';
   Log(szNow);

   // What --grant-database-login said before the move, when it could not give the
   // account its login: the statements a SQL Server administrator runs.
   if g_szDatabaseLoginSaid <> '' then
      szNow := szNow + #13#10#13#10 + g_szDatabaseLoginSaid;

   SuppressibleMsgBox('The hMailServer service was moved to NT SERVICE\hMailServer, and as that account ' + szWhat + '. ' + szNow + #13#10#13#10 +
          'The hMailServer error log and the Windows Application log say why. Where the database is SQL Server with Windows authentication, "hMailServer.exe --grant-database-login", ' +
          'run from an elevated prompt in the hMailServer Bin folder by a SQL Server administrator, gives NT SERVICE\hMailServer its login and user there, or says what to run. ' +
          'Then move the service from the Control Panel (Windows service account), or run this setup again with the move chosen (/serviceaccount=virtual for a silent one).', mbError, MB_OK, IDOK);
end;

// What retries a database step DBSetupQuick did not complete. On an upgrade,
// DBSetupQuick.exe: nothing after that step is a new installation's. On a new
// installation, this setup again, which finishes it (PendingNewInstallation): the
// secure defaults, the profile, the stored-secrets key and the service's account
// all come after that step, and DBSetupQuick.exe sets the database up and nothing
// after it.
function QuickStepRetryAdvice(): String;
begin
   if IsNewDatabase() then
      Result := 'Run this setup again, which finishes this new installation as it would have: the secure defaults, the stored-secrets key and the service''s account.'
   else
      Result := 'Run DBSetupQuick.exe from the hMailServer Bin folder to retry.';
end;


function RunPostInstallTasks() : Boolean;
   var
      ResultCode: Integer;
      ProgressPage : TOutputProgressWizardPage;
      szParameters: AnsiString;
      szCredential: String;
      szDBSetupSaid: String;
      szQuickReport: String;
      szQuickSaid: String;
      szNotComplete: String;
      bRuntimeReady: Boolean;
      bDatabaseBackendReady: Boolean;
      bServiceRegistered: Boolean;
      bPasswordWritten: Boolean;
      bStarted: Boolean;
      bStopBeforeDatabase: Boolean;
      bDatabaseStepOnExisting: Boolean;
      bMoved: Boolean;
      szCreate: String;
begin
   Result := true;
   bRuntimeReady := true;
   bStopBeforeDatabase := false;
   bDatabaseStepOnExisting := false;
   bMoved := false;

   // A database given for a new installation, to one that has a database already:
   // an upgrade keeps the database it has, which the setup log says - and so does
   // a new installation an earlier run left unfinished, once DBSetup has set its
   // database up.
   if g_bDatabaseGiven and (GetCurrentDatabaseType() <> '') then
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

         // The program's folders of mail, logs, keys and scripts, and
         // hMailServer.INI, private to the administrators and the service - at
         // every installation and upgrade, whatever account the service runs as,
         // and before the database step makes or opens anything in them.
         ProgressPage.SetText('Making the program''s data, log and database folders private, and its program folders the administrators'' to change...', 'On a large message store the first time takes a few minutes.');
         ProtectFolders();

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

         // The mark an earlier run left on a new installation it did not finish is
         // this installation's to finish now (PendingNewInstallation): cleared
         // before the database step, so that the DBSetup it runs leaves the steps
         // after it to this installation, and written again only if this one does
         // not complete that step either (CurStepChanged).
         if PendingNewInstallation() then
            ClearPendingNewInstallation();

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
            // Existing is what the settings file names before the step, not
            // IsNewDatabase: an unfinished new installation's database that DBSetup
            // or DBSetupQuick has set up since is finished as a new one's
            // (PendingNewInstallation), and is still a database this step can
            // change - exit code 11, not 10, when it did.
            bDatabaseStepOnExisting := (GetCurrentDatabaseType() <> '');
            if bDatabaseStepOnExisting then
               BeginDatabaseStep();

            // An existing database - an upgrade, or a repair over one - meets the
            // new binary here, before DBUpdater has moved it: the service is
            // started with the argument that says so (StartServiceForDatabaseUpgrade).
            // A new installation has no database for the service to find yet.
            if bDatabaseStepOnExisting and (bServiceRegistered = true) then
               StartServiceForDatabaseUpgrade();

            ResultCode := 0;
            bStarted := false;
            szQuickSaid := '';
            // What DBSetupQuick says about a failure goes into a file of this
            // installation's (report:PATH, on its command line), which is copied
            // into this log below: it is a Windows program, whose output nothing
            // here could read. Until 6.3.6 it said nothing anywhere, and a service
            // it could not reach killed it with -532462766.
            szQuickReport := ExpandConstant('{tmp}\dbsetupquick-report.txt');
            DeleteFile(szQuickReport);
            if not SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', szCredential) then
               SuppressibleMsgBox('The hMailServer database step could not be started: the administrator password could not be handed to DBSetupQuick.exe. ' + QuickStepRetryAdvice(), mbError, MB_OK, IDOK)
            else
            begin
               try
                  bStarted := Exec(ExpandConstant('{app}\Bin\DBSetupQuick.exe'), Trim(szParameters + ' "report:' + szQuickReport + '"'), '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode);
                  if (bStarted = False) then
                     SuppressibleMsgBox(SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               finally
                  SetProcessEnvironmentVariable('HM_ADMIN_PASSWORD', '');
               end;
               if bStarted then
                  szQuickSaid := ReadDBSetupQuickReport(szQuickReport, ResultCode);
            end;

            // The database's version now, whatever the step's result: the step
            // can fail after it has moved it - a chain of steps on MySQL, whose DDL
            // commits as it goes, or the service's own start with UpgradeOnStart=1.
            // Exit code 4 is DBSetupQuick's for a service it could not reach, so
            // that neither it nor DBUpdater ran (DBSetupQuick's ExitServiceNotReached).
            if bDatabaseStepOnExisting then
               EndDatabaseStep(bStarted and (ResultCode = 4));

            if (bStarted = False) then
            begin
               Result := false;
               if bDatabaseStepOnExisting and g_bDatabaseChanged then
                  SayDatabaseStepStoppedPartWay();
            end
            else if (ResultCode = 4) then
            begin
               SuppressibleMsgBox('The hMailServer database step could not run: DBSetupQuick.exe could not reach the hMailServer service (exit code 4), so neither it nor DBUpdater changed the database.' + #13#10#13#10 +
                      szQuickSaid + #13#10#13#10 +
                      'Check that the hMailServer service starts - the Windows Application log and the hMailServer error log say why it does not. ' + QuickStepRetryAdvice(), mbError, MB_OK, IDOK);
               Result := false;
               if bDatabaseStepOnExisting and g_bDatabaseChanged then
                  SayDatabaseStepStoppedPartWay();
            end
            else if (ResultCode <> 0) then
            begin
               if szQuickSaid <> '' then
                  szQuickSaid := #13#13 + szQuickSaid;
               SuppressibleMsgBox('The hMailServer database could not be created or upgraded (exit code ' + IntToStr(ResultCode) + ').' + szQuickSaid + #13#13
                      'The hMailServer service may not work until the database has been created or upgraded. ' + QuickStepRetryAdvice(), mbError, MB_OK, IDOK);
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
               // The built-in database DBSetupQuick has just made, as opposed to
               // an external one DBSetup set up after an earlier run of this setup
               // did not finish (PendingNewInstallation), which this step found.
               ApplyNewInstallDefaults(GetCurrentDatabaseType() = 'mssqlce');
               ProgressPage.SetText('Applying the server profile...', '');
               ApplyChosenProfile();
               ProgressPage.SetText('Making the stored-secrets key...', '');
               CreateStoredSecretsKey();
               g_bNewInstallationFinished := true;
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
            g_bDatabaseStepByDBSetup := true;
            if g_bDatabaseGiven then
            begin
               // Whether it creates the database, as DBSetup reads the setting:
               // /createdatabase=, or HM_CREATE_DATABASE, 1 unless 0, no or false.
               szCreate := DatabaseSwitch('createdatabase');
               if szCreate = '' then
                  szCreate := Trim(GetEnv('HM_CREATE_DATABASE'));
               szCreate := Lowercase(szCreate);

               // The database given with /dbtype or HM_DB_TYPE, set up with no
               // window (RunUnattendedDBSetup), silent installation or not.
               bStarted := RunUnattendedDBSetup(szCredential, ResultCode, szDBSetupSaid);
               g_bDatabaseCreatedHere := bStarted and (ResultCode = 0) and (szCreate <> '0') and (szCreate <> 'no') and (szCreate <> 'false');
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
                      'The hMailServer service may not work until the database has been set up. Correct the database settings (docs/WindowsOnArm.md) and run this setup again; ' +
                      'or run DBSetup.exe as an administrator from the hMailServer Bin folder - /unattended with the settings in its environment, or its wizard. ' +
                      'Either finishes this new installation as this setup would have: the secure defaults, the stored-secrets key and the service''s account.', mbError, MB_OK, IDOK);
               Result := false;
            end
            else if (ResultCode <> 0) then
            begin
               SuppressibleMsgBox('The hMailServer database setup did not complete (exit code ' + IntToStr(ResultCode) + ').' #13#13
                      'The hMailServer service may not work until the database has been set up. Run this setup again, or DBSetup.exe as an administrator from the hMailServer Bin folder: ' +
                      'either finishes this new installation as this setup would have - the secure defaults, the stored-secrets key and the service''s account.', mbError, MB_OK, IDOK);
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
               g_bNewInstallationFinished := true;
            end;
         end;

         // The move to the virtual account, once the database is there.
         // Not after a database step that failed: the service stays LocalSystem's,
         // where DBSetup can be run again - which, on a new installation, finishes
         // it, the move included (PendingNewInstallation) - and is moved from the
         // Control Panel. A service that could not be given its login on SQL
         // Server with Windows authentication is not moved, and fails the
         // installation (GrantServiceDatabaseLogin).
         if bServiceRegistered and Result and MoveServiceAfterDatabase(GetServiceAccountChoice()) then
         begin
            ProgressPage.SetText('Moving the service to NT SERVICE\hMailServer...', 'Granting its folders; on a large message store this takes a few minutes.');
            bMoved := MoveServiceToVirtualAccount();
            if g_bDatabaseLoginFailed then
               Result := false;
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
            if g_bFinishNewInstallationAtStart then
               bStarted := StartServiceToFinishNewInstallation(ResultCode)
            else
               bStarted := Exec(ExpandConstant('{sys}\net.exe'), 'START hMailServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

            if (bStarted = False) then
            begin
               SuppressibleMsgBox('The hMailServer service could not be started.' + #13#10 + SysErrorMessage(ResultCode), mbError, MB_OK, IDOK);
               Result := false;
            end
            // A service this installation moved is held to opening its database as
            // the virtual account, and moved back to LocalSystem when it does not.
            else if bMoved then
            begin
               if (CheckServiceAfterMove() = false) then
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

		// A folder every user of the computer may change what is in - one at a
		// drive's root - said, never refused (CheckProgramFolderPermissions).
		if Result then
			CheckProgramFolderPermissions(WizardDirValue(), true);
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
	   // Whether this installation makes the program folder, for the permissions
	   // it gives it before the files (HardenNewProgramFolder); and in the setup
	   // log, whether every user may change what the folder holds, for an
	   // installation whose folder page was not shown - a silent one, or an
	   // upgrade (CheckProgramFolderPermissions).
	   g_bProgramFolderNew := not DirExists(ExpandConstant('{app}'));
	   CheckProgramFolderPermissions(ExpandConstant('{app}'), false);

	   // An installation of the Control Panel without the server, into a folder
	   // that was there: hardened now, before the files, since no /ProtectFolders
	   // follows them (HardenExistingProgramFolders).
	   HardenExistingProgramFolders();

	   // Whether the database is new, asked of the settings file before the
	   // installation writes one, and of the mark an unfinished new installation
	   // leaves, before this one clears it (IsNewDatabase, PendingNewInstallation).
	   IsNewDatabase();

	   // And the account the service ran as, before this installation registers
	   // it again (GetServiceAccountBefore), and whether that makes this a new
	   // installation's service, before the database step names a database in
	   // the settings file (IsNewService).
	   GetServiceAccountBefore();
	   IsNewService();

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
	    // A new installation whose database step did not complete leaves the mark
	    // that lets this setup run again, or DBSetup, finish it as a new one
	    // (PendingNewInstallation): the secure defaults, the profile, the
	    // stored-secrets key and the move to NT SERVICE\hMailServer all come after
	    // that step.
	    if (RunPostInstallTasks() = false) then
	    begin
	      g_bStepsIncomplete := true;
	      if IsNewDatabase() and (g_bNewInstallationFinished = false) then
	        RecordPendingNewInstallation(GetServiceAccountChoice(), GetChosenProfile());
	      if g_bDatabaseChanged then
	        RaiseException('A post-installation step did not complete after the database step had changed the database, so ' + PreviousVersionText() + ' is not to be put back on it: it may not run on that database. See the message above.')
	      else if g_bGrantFailed then
	        RaiseException('The hMailServer service account could not be granted its folders. See the message above; run "hMailServer.exe --confirm /GrantServiceAccess" as an administrator from the hMailServer Bin folder to retry.')
	      else if g_bDatabaseLoginFailed then
	        RaiseException('The hMailServer service runs as LocalSystem, not NT SERVICE\hMailServer: that account cannot open the database. See the message above; once a SQL Server administrator has run what it says, move the service from the Control Panel, or run this setup again with the move chosen.')
	      // A new installation's external database, which DBSetup sets up:
	      // DBSetupQuick has nothing to retry there, and without SQL Server Compact,
	      // which such an installation does not install, it does not even start.
	      else if g_bDatabaseStepByDBSetup then
	        RaiseException('A post-installation step did not complete: the administrator password, the service or the database. See the message above; run this setup again, or DBSetup.exe as an administrator from the hMailServer Bin folder, to set the database up and finish the installation.')
	      // A new installation on the built-in database whose steps after the
	      // database did not run: this setup again finishes it, where DBSetupQuick
	      // would set the database up and nothing after it (QuickStepRetryAdvice).
	      else if IsNewDatabase() and (g_bNewInstallationFinished = false) then
	        RaiseException('A post-installation step did not complete: the administrator password, the service or the database. See the message above; run this setup again, which finishes the new installation.')
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
      // The mark of a new installation an earlier run left unfinished
      // (PendingNewInstallation): nothing is left to finish once the program is
      // gone, and a later installation must not take its account for its own.
      ClearPendingNewInstallation();

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


