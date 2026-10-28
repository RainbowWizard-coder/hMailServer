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

  // Set in PrepareToInstall when this installation made the program folder, with
  // its permissions from the start (MakeFolders): section_dirs.iss then gives
  // the uninstaller the folder to remove (SetupMadeProgramFolder).
  g_bProgramFolderNew: Boolean;
  // Work item 78's R1 to R4 (PrepareProgramFolders): every folder held open from
  // the root of its volume down to the program folder, Bin and ControlPanel, kept
  // until DeinitializeSetup lets them go; the folders this setup made, shallowest
  // first; the program folder where its links lead; what an administrator is to
  // look at, for the box an attended installation shows; and whether the files were
  // installed, after which DeinitializeSetup removes nothing setup made.
  g_aHeldFolders: array of Integer;
  g_aFoldersMade: array of String;
  g_szProgramFolderReal: String;
  g_szFolderFindings: String;
  g_bFilesInstalled: Boolean;
  // What CheckProgramFolders found and holds, which PrepareProgramFolders makes or
  // hardens without looking at a path again: for the program folder, Bin and
  // ControlPanel each, where its walk ended (Real), the deepest folder of it found and
  // held (Held), the handle that holds that one, and whether that is the folder itself
  // (There); whether the program folder is the earlier installation's; Bin and
  // ControlPanel when setup made them in a program folder that was there; the {app}
  // the check was of, whether it has run since PrepareToInstall last did, and its
  // refusal; and whether setup stopped a running service, which a refusal then says.
  g_szProgramFolderHeld: String;
  g_hProgramFolder: Integer;
  g_bProgramFolderThere: Boolean;
  g_szBinReal: String;
  g_szBinHeld: String;
  g_hBin: Integer;
  g_bBinThere: Boolean;
  g_szControlPanelReal: String;
  g_szControlPanelHeld: String;
  g_hControlPanel: Integer;
  g_bControlPanelThere: Boolean;
  g_bEarlierInstallation: Boolean;
  g_szBinMade: String;
  g_szControlPanelMade: String;
  g_szFoldersCheckedFor: String;
  g_bFoldersChecked: Boolean;
  g_szFolderRefusal: String;
  g_bServiceStoppedForSetup: Boolean;
  // The last folder CheckProgramFolderPermissions looked at, so that one choice
  // is said once.
  g_szProgramFolderChecked: String;
  // The program folder where its walk ended, once that walk is done: Bin's and
  // ControlPanel's walks do not hold the folders at it and above it to the rule of a
  // folder above the program folder again (IsAtOrAboveChecked); '' during the
  // program folder's own walk.
  g_szFolderAboveChecked: String;
  // What PrepareProgramFolders left, for the check after the files
  // (FoldersStillAsPrepared): the handles that hold the program folder, Bin and
  // ControlPanel themselves once they are made or hardened (-1 for one setup did not
  // make, harden or hold, as a Bin Inno Setup makes in a new program folder), and
  // whether the program folder was made or hardened, not left to /ProtectFolders.
  g_hProgramFolderPrepared: Integer;
  g_hBinPrepared: Integer;
  g_hControlPanelPrepared: Integer;
  g_bProgramFolderHardened: Boolean;
  // Work item 78's R2, decided by CheckProgramFolders before anything changes: whether
  // Bin and ControlPanel were there and held nothing, which PrepareProgramFolders then
  // removes and makes again rather than hardens, whoever could write it - asked before
  // the program folder is hardened, whose new permissions Windows hands down to them.
  g_bRemakeBin: Boolean;
  g_bRemakeControlPanel: Boolean;
  // Whether PrepareProgramFolders leaves the program folder unhardened before the files,
  // and for which data folder (ProgramFolderWaitsForTheFiles) - asked once, in
  // CheckProgramFolders, before anything changes, so that the restart rule and
  // PrepareProgramFolders go by the same answer; PrepareProgramFolders asks again and
  // refuses when the answer is not the same (check 7 of R1 to R4, 28 October 2026).
  g_szProgramFolderWaits: String;
  // For each folder this setup made (MakeFolders), recorded once it was found where it
  // was named: where it was made, read through the handle that holds it
  // (FinalPathOfHeld), and that handle - g_aFoldersMade's, Bin's and ControlPanel's -
  // so that it is removed only while it is still that folder (RemoveMadeFolder).
  g_aFoldersMadeAt: array of String;
  g_aFoldersMadeHandle: array of Integer;
  g_szBinMadeAt: String;
  g_hBinMade: Integer;
  g_szControlPanelMadeAt: String;
  g_hControlPanelMade: Integer;
  // Set when the folders setup prepared were not as it left them after the files
  // (FoldersStillAsPrepared): the last page then runs nothing (FoldersWereAsPrepared).
  g_bFoldersNotAsPrepared: Boolean;
  // Work item 78's R2 and R3, decided by CheckProgramFolders before anything changes:
  // the folders of the program there before this installation that another account
  // could change - their permissions or owner, or what is in them - whose handles such
  // an account opened then keep that right after setup hardens them, each with what it
  // let and whom (HardenForRestart); and the files setup keeps in a folder it hardened
  // in place until the files are in (MakePlaceholder), let go at ssPostInstall and in
  // DeinitializeSetup.
  g_aRestartFolders: array of String;
  g_aRestartHandles: array of Integer;
  g_szRestartWhy: String;
  g_aPlaceholders: array of Integer;
  // For each of g_aRestartFolders, whether it is a data folder - Data, Logs, Temp or
  // Database of the earlier installation, or where such a folder's link leads - that
  // another account owned, which HardenForRestart takes keeping the service's entries
  // (HardenDataFolder); and a program, a library, a script or a settings file there before
  // this installation that an entry it inherited where it was before, and which the folder
  // it is in does not hand down, let another account change, with what it let and whom
  // (NoteFolderOthersCouldChange) - check 5 of R1 to R4, 27 October 2026.
  g_aRestartData: array of Boolean;
  // For each of g_aRestartFolders, whether its owner, or an entry setup's hardening takes
  // away, let another account change its permissions or its owner (OthersCouldChange): an
  // entry of its own for a named account may then be one that account wrote through that
  // right, which HardenForRestart does not keep (HardenDistrustedFolder) - check 6 of R1
  // to R4, 28 October 2026.
  g_aRestartDistrust: array of Boolean;
  g_aStaleItems: array of String;
  g_aStaleWhat: array of String;
  // Set when PrepareProgramFolders, after the service was stopped, found a folder it
  // prepares changed - made a link, or something put in it - which PrepareToInstall
  // then refuses with the service set not to start, as after the files.
  g_bFoldersChanged: Boolean;

// The hMailServer service is looked at and stopped through Windows' Service Control
// Manager, as Microsoft documents it (learn.microsoft.com, "Service Functions" and
// winsvc.h): OpenSCManagerW connects to the manager of this computer, OpenServiceW
// opens one service by its name with the access a caller needs, QueryServiceStatusEx
// reads its state, ControlService sends it the stop, and CloseServiceHandle lets go
// of each handle. The types, constants and declarations below restate that API; the
// routines built on it, OpenServiceManager and those after it, follow DecodeVersion.

type
	// A Windows handle: pointer-sized, so 32 bits in this 32-bit setup. SC_HANDLE is
	// the kind the Service Control Manager gives out, for itself and for a service,
	// and takes back with CloseServiceHandle; 0 is no handle.
	HANDLE = cardinal;
	SC_HANDLE = HANDLE;
	// SERVICE_STATUS, which ControlService fills in for the service it sent a
	// control to: seven DWORDs, in winsvc.h's order. Nothing here reads it; the
	// state is read through QueryServiceStatusEx (SERVICE_STATUS_PROCESS below).
	SERVICE_STATUS = record
		dwServiceType: cardinal;
		dwCurrentState: cardinal;
		dwControlsAccepted: cardinal;
		dwWin32ExitCode: cardinal;
		dwServiceSpecificExitCode: cardinal;
		dwCheckPoint: cardinal;
		dwWaitHint: cardinal;
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
	// OBJECT_ATTRIBUTES, UNICODE_STRING and IO_STATUS_BLOCK, named apart from them, for a
	// folder made relative to the handle of the folder above it (MakeFolderIn): this setup
	// is a 32-bit program, so that each pointer and handle is four bytes and
	// OBJECT_ATTRIBUTES is 24 (HM_OBJECT_ATTRIBUTES_SIZE) - its size, the handle the name is
	// relative to, the name, OBJ_CASE_INSENSITIVE, the security descriptor the folder is
	// made with, and none for quality of service; UNICODE_STRING's two lengths, two Words,
	// are one cardinal here, the length in bytes in its low half and the room in its high,
	// then the characters.
	HM_OBJECT_ATTRIBUTES = record
		nLength						: cardinal;
		RootDirectory				: Integer;
		ObjectName					: cardinal;
		Attributes					: cardinal;
		SecurityDescriptor			: cardinal;
		SecurityQualityOfService	: cardinal;
	end;
	HM_UNICODE_STRING = record
		Lengths						: cardinal;
		Buffer						: cardinal;
	end;
	HM_IO_STATUS_BLOCK = record
		Status						: cardinal;
		Information					: cardinal;
	end;
	// BY_HANDLE_FILE_INFORMATION, named apart from it: what a handle says of the
	// folder it holds - its attributes first (HeldIsLink) - thirteen DWORDs, 52 bytes.
	HM_FILE_INFORMATION = record
		dwFileAttributes			: cardinal;
		ftCreationTimeLow			: cardinal;
		ftCreationTimeHigh			: cardinal;
		ftLastAccessTimeLow			: cardinal;
		ftLastAccessTimeHigh		: cardinal;
		ftLastWriteTimeLow			: cardinal;
		ftLastWriteTimeHigh			: cardinal;
		dwVolumeSerialNumber		: cardinal;
		nFileSizeHigh				: cardinal;
		nFileSizeLow				: cardinal;
		nNumberOfLinks				: cardinal;
		nFileIndexHigh				: cardinal;
		nFileIndexLow				: cardinal;
	end;
	
const
	// The Service Control Manager's access rights, controls and states that the
	// routines below use, with winsvc.h's values ("Service Security and Access
	// Rights", ControlService, SERVICE_STATUS). SC_MANAGER_CONNECT is all that
	// opening a service by its name asks of the manager; what may be done to the
	// service is checked on the service, by the right it is opened with.
	SC_MANAGER_CONNECT          = $0001;
	SERVICE_QUERY_CONFIG        = $0001;
	SERVICE_QUERY_STATUS        = $0004;
	SERVICE_STOP                = $0020;
	SERVICE_CONTROL_STOP        = $00000001;
	SERVICE_STOPPED             = $00000001;
	SERVICE_RUNNING             = $00000004;

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
	// Reading a file or listing a folder (FILE_LIST_DIRECTORY), or GENERIC_READ or
	// GENERIC_ALL unmapped, each of which a folder maps to it (EveryUserMayRead,
	// EveryUserMayReadMadeIn; ServiceAccount.cpp's EveryUserReads_, which maps an
	// entry's generic rights before it asks for FILE_READ_DATA). An entry handed down
	// to what is made in a folder is often written with generic rights - Program
	// Files gives Users (OI)(CI)(IO)(GR,GE), $A0000000 - so a mask that left out
	// GENERIC_READ would find no user who may read a folder made there. Each mask
	// here that an entry's rights are tested against carries exactly the generic
	// rights whose mapping for a file or folder (FILE_GENERIC_READ $120089,
	// FILE_GENERIC_WRITE $120116, FILE_GENERIC_EXECUTE $1200A0, FILE_ALL_ACCESS
	// $1F01FF) meets one of its own: GENERIC_EXECUTE meets none of them.
	HM_FOLDER_READ_RIGHTS       = $90000001;
	// For the program folder, Bin and ControlPanel, held from the root of their
	// volume down while setup prepares and installs them (HoldForSetup,
	// HoldFolderChain): a folder held open with FILE_LIST_DIRECTORY | FILE_TRAVERSE |
	// FILE_READ_ATTRIBUTES | READ_CONTROL - a handle that asks for the attributes
	// alone takes no part in Windows' check of sharing, and its refusal to share
	// deleting would bind nobody (ProtectedFile.cpp's HeldFolder holds a folder the
	// same way); READ_CONTROL, so that a link's owner and permissions are read through
	// the handle that holds it - shared for reading and writing but not for deleting
	// (FILE_SHARE_READ | FILE_SHARE_WRITE: a file renamed into a folder, as setup
	// copies one, opens the folder for writing), OPEN_EXISTING, and
	// FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, so that a link is
	// opened itself, not followed; READ_CONTROL alone and every sharing, for reading
	// the owner of what is in a folder; FILE_ATTRIBUTE_REPARSE_POINT and
	// INVALID_FILE_ATTRIBUTES; the owner alone (OWNER_SECURITY_INFORMATION), the
	// owner and the list (OWNER_ | DACL_SECURITY_INFORMATION), and a list that
	// inherits nothing (DACL_ | PROTECTED_DACL_SECURITY_INFORMATION), which a folder's
	// owner may always write.
	HM_HOLD_FOLDER_ACCESS       = $200A1;
	HM_READ_CONTROL             = $20000;
	HM_SHARE_ALL_BUT_DELETE     = $3;
	HM_SHARE_EVERYTHING         = $7;
	HM_OPEN_EXISTING            = 3;
	HM_HOLD_FOLDER_FLAGS        = $02200000;
	HM_REPARSE_POINT            = $400;
	HM_INVALID_ATTRIBUTES       = $FFFFFFFF;
	HM_OWNER_INFORMATION        = 1;
	HM_OWNER_AND_DACL_INFORMATION = 5;
	HM_PROTECTED_DACL_INFORMATION = $80000004;
	// For a link on the way to the program folder (LinkIsAdministratorsAlone,
	// LinkTarget): what removes a name from the folder it is in or takes that
	// folder's permissions - FILE_DELETE_CHILD, WRITE_DAC, WRITE_OWNER, or
	// GENERIC_ALL unmapped (ProtectedFile.cpp's OnlyAdministratorsControl, its
	// folder); FSCTL_GET_REPARSE_POINT; DRIVE_REMOTE; and the SIDs of TrustedInstaller
	// and of the service's virtual account, NT SERVICE\hMailServer
	// (ServiceAccount::ServiceSid, which ServiceAccountTester holds to this value).
	HM_REMOVE_FROM_FOLDER_RIGHTS = $100C0040;
	// For a folder above the program folder (FolderAboveIsAdministratorsAlone): what
	// lets the next folder down be replaced - removing a name from it
	// (FILE_DELETE_CHILD), renaming or removing it (DELETE), or changing who may
	// (WRITE_DAC, WRITE_OWNER), or GENERIC_ALL unmapped: AdminUpdateChecks.cpp's
	// ReplaceRights_, which the live update holds every folder above the program
	// folder to.
	HM_REPLACE_RIGHTS           = $100D0040;
	HM_GET_REPARSE_POINT        = $000900A8;
	HM_DRIVE_REMOTE             = 4;
	HM_TRUSTED_INSTALLER_SID    = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464';
	HM_SERVICE_SID              = 'S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809';
	// For a folder made with permissions of its own (MakeFolders, MakeFolderIn) and one
	// rebuilt (RebuildFolderOfUntrustedOwner): STATUS_OBJECT_NAME_COLLISION, NtCreateFile's
	// answer for a name somebody else has taken; STATUS_NO_MEMORY; the access a folder is
	// made and held with, HM_HOLD_FOLDER_ACCESS's with SYNCHRONIZE, which a synchronous open
	// needs; FILE_CREATE; FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
	// FILE_OPEN_REPARSE_POINT; OBJ_CASE_INSENSITIVE; OBJECT_ATTRIBUTES' size; ACL_REVISION;
	// LPTR; MAXDWORD, AddAce's "at the end"; ACCESS_DENIED_ACE_TYPE; INHERITED_ACE,
	// and the same bit where it sits in ACE_START's TypeFlagsSize; FILE_ALL_ACCESS;
	// Read & execute (ServiceAccount::RightsReadExecute); SE_DACL_PROTECTED; and
	// FILE_PERSISTENT_ACLS, which a volume that keeps permissions has.
	HM_STATUS_NAME_COLLISION    = $C0000035;
	HM_STATUS_NO_MEMORY         = $C0000017;
	HM_MAKE_FOLDER_ACCESS       = $1200A1;
	HM_FILE_CREATE              = 2;
	HM_MAKE_FOLDER_OPTIONS      = $00200021;
	HM_OBJ_CASE_INSENSITIVE     = $40;
	HM_OBJECT_ATTRIBUTES_SIZE   = 24;
	HM_ACL_REVISION             = 2;
	HM_LPTR                     = $40;
	HM_MAXDWORD                 = $FFFFFFFF;
	HM_ACCESS_DENIED            = 1;
	HM_INHERITED_ACE            = $10;
	HM_INHERITED_FLAG_BIT       = $1000;
	HM_FILE_ALL_ACCESS          = $1F01FF;
	HM_RIGHTS_READ_EXECUTE      = $1200A9;
	HM_DACL_PROTECTED           = $1000;
	HM_PERSISTENT_ACLS          = $8;
	// GetFinalPathNameByHandleW's VOLUME_NAME_GUID with FILE_NAME_NORMALIZED: the path
	// a held folder is at now, as \\?\Volume{...}\..., whatever drive letter or mount
	// point it is reached by (FinalPathOfHeld).
	HM_VOLUME_NAME_GUID         = 1;
	// Work item 78's R2 and R3, what setup asks of a folder of the program that was there
	// before it, whose permissions it changes in place (CheckProgramFolders,
	// HardenForRestart): what changes its permissions or its owner - WRITE_DAC,
	// WRITE_OWNER, or GENERIC_ALL unmapped - which a handle opened while an entry gave it
	// keeps after any change of the permissions, and so may give back whatever setup
	// takes away. And, for that, the volatile key setup leaves until Windows restarts
	// (RecordRestartNeeded, RestartNeededFor): HKEY_LOCAL_MACHINE, REG_OPTION_VOLATILE,
	// KEY_SET_VALUE | KEY_WOW64_64KEY, REG_SZ and the key's name; and the file setup keeps
	// in a folder it hardened in place until its files are in (MakePlaceholder):
	// GENERIC_READ | DELETE, CREATE_NEW, and FILE_FLAG_DELETE_ON_CLOSE |
	// FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN, so that it is gone the moment setup
	// lets it go, or setup ends however it ends.
	HM_CHANGE_PERMISSIONS_RIGHTS = $100C0000;
	HM_HKEY_LOCAL_MACHINE       = $80000002;
	HM_REG_OPTION_VOLATILE      = 1;
	HM_KEY_SET_VALUE_64         = $0102;
	HM_REG_SZ                   = 1;
	HM_RESTART_KEY              = 'SOFTWARE\hMailServerSetupRestart';
	HM_PLACEHOLDER_ACCESS       = $80010000;
	HM_CREATE_NEW               = 1;
	HM_PLACEHOLDER_FLAGS        = $04000102;
	HM_PLACEHOLDER_NAME         = 'hMailServer-setup-in-progress.tmp';
	// An empty folder removed through a handle opened on it as itself (RemoveHeldFolder):
	// DELETE | FILE_READ_ATTRIBUTES; FileDispositionInfo and FileDispositionInfoEx; and
	// FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS, which takes the
	// name away at once, whoever else holds the folder open. And a folder opened through
	// its links, for where it is (FinalPathOf): FILE_FLAG_BACKUP_SEMANTICS alone.
	HM_REMOVE_FOLDER_ACCESS     = $00010080;
	HM_FILE_DISPOSITION_INFO    = 4;
	HM_FILE_DISPOSITION_INFO_EX = 21;
	HM_DISPOSITION_DELETE_POSIX = 3;
	HM_OPEN_FOLLOWING_FLAGS     = $02000000;

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

// The Service Control Manager's functions, as winsvc.h declares them, through their
// Unicode entry points where a name is passed: a String reaches a W function as a
// pointer to its wide characters. OpenSCManagerW's two names are pointers passed as
// cardinals, so that 0 is NULL: the manager of this computer, and its database of
// the services installed (SERVICES_ACTIVE_DATABASE). BOOL results are read as
// Boolean, as everywhere in this script.
function OpenSCManagerW(lpMachineName: cardinal; lpDatabaseName: cardinal; dwDesiredAccess: cardinal): SC_HANDLE;
external 'OpenSCManagerW@advapi32.dll stdcall';

function OpenServiceW(hSCManager: SC_HANDLE; lpServiceName: String; dwDesiredAccess: cardinal): SC_HANDLE;
external 'OpenServiceW@advapi32.dll stdcall';

function ControlService(hService: SC_HANDLE; dwControl: cardinal; var lpServiceStatus: SERVICE_STATUS): Boolean;
external 'ControlService@advapi32.dll stdcall';

function CloseServiceHandle(hSCObject: SC_HANDLE): Boolean;
external 'CloseServiceHandle@advapi32.dll stdcall';

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

// The program folder, Bin and ControlPanel, held and made or hardened before the
// files (PrepareProgramFolders): CreateFileW to hold each folder from the drive's
// root down while it is looked at and changed, CloseHandle to let it go - as
// Integer, so that INVALID_HANDLE_VALUE reads -1 - GetFileAttributesW to know a
// link, and ConvertStringSecurityDescriptorToSecurityDescriptorW,
// GetSecurityDescriptorDacl, GetSecurityDescriptorOwner and SetNamedSecurityInfoW
// to make a descriptor and give a folder a new list. BOOL out parameters are
// cardinals, which is their width. Named apart from the API for the reason
// SetProcessEnvironmentVariable is.
function HoldFolder(lpFileName: String; dwDesiredAccess: cardinal; dwShareMode: cardinal; lpSecurityAttributes: cardinal;
                    dwCreationDisposition: cardinal; dwFlagsAndAttributes: cardinal; hTemplateFile: cardinal): Integer;
external 'CreateFileW@kernel32.dll stdcall';
function ReleaseFolder(hObject: Integer): Boolean;
external 'CloseHandle@kernel32.dll stdcall';
function FolderAttributes(lpFileName: String): cardinal;
external 'GetFileAttributesW@kernel32.dll stdcall';
function SecurityDescriptorOfText(StringSecurityDescriptor: String; StringSDRevision: cardinal; var SecurityDescriptor: cardinal; SecurityDescriptorSize: cardinal): Boolean;
external 'ConvertStringSecurityDescriptorToSecurityDescriptorW@advapi32.dll stdcall';
function DaclOfDescriptor(pSecurityDescriptor: cardinal; var lpbDaclPresent: cardinal; var pDacl: cardinal; var lpbDaclDefaulted: cardinal): Boolean;
external 'GetSecurityDescriptorDacl@advapi32.dll stdcall';
function OwnerOfDescriptor(pSecurityDescriptor: cardinal; var pOwner: cardinal; var lpbOwnerDefaulted: cardinal): Boolean;
external 'GetSecurityDescriptorOwner@advapi32.dll stdcall';
function WriteFolderSecurity(pObjectName: String; ObjectType: cardinal; SecurityInfo: cardinal; psidOwner: cardinal; psidGroup: cardinal;
                             pDacl: cardinal; pSacl: cardinal): cardinal;
external 'SetNamedSecurityInfoW@advapi32.dll stdcall';

// Work item 78's R1 to R4 (PrepareProgramFolders): GetSecurityInfo, to read a held
// folder's or link's owner and permissions through its handle;
// GetSecurityDescriptorControl, for whether a list inherits; ConvertSidToStringSidW,
// lstrlenW and RtlMoveMemory into a String, for a SID in what setup says;
// RtlMoveMemory between pointers, LocalAlloc, InitializeAcl, AddAce and
// AddAccessAllowedAceEx, to rebuild a list (RebuildFolderOfUntrustedOwner);
// DeviceIoControl with FSCTL_GET_REPARSE_POINT into a String, for where a link leads;
// NtCreateFile, with RtlMoveMemory from a String and from a record, to make a folder with
// permissions of its own from the start, by its name relative to the handle of the folder
// above it (MakeFolderIn); GetVolumeInformationByHandleW, for whether a volume keeps
// permissions; GetDriveTypeW; and LookupAccountSidW, for the name of an account in
// what setup says. Named apart from the API for the reason
// SetProcessEnvironmentVariable is.
function ReadHeldSecurity(Handle: Integer; ObjectType: cardinal; SecurityInfo: cardinal; var ppsidOwner: cardinal; ppsidGroup: cardinal;
                          var ppDacl: cardinal; ppSacl: cardinal; var ppSecurityDescriptor: cardinal): cardinal;
external 'GetSecurityInfo@advapi32.dll stdcall';
function ControlOfDescriptor(pSecurityDescriptor: cardinal; var pControl: Word; var lpdwRevision: cardinal): Boolean;
external 'GetSecurityDescriptorControl@advapi32.dll stdcall';
function SidToText(Sid: cardinal; var StringSid: cardinal): Boolean;
external 'ConvertSidToStringSidW@advapi32.dll stdcall';
function WideTextLength(lpString: cardinal): Integer;
external 'lstrlenW@kernel32.dll stdcall';
procedure CopyWideText(Destination: String; Source: cardinal; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
procedure CopyBytes(Destination: cardinal; Source: cardinal; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
procedure WriteAceStart(Destination: cardinal; var Source: ACE_START; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
function AllocateWindowsMemory(uFlags: cardinal; uBytes: cardinal): cardinal;
external 'LocalAlloc@kernel32.dll stdcall';
function StartAcl(pAcl: cardinal; nAclLength: cardinal; dwAclRevision: cardinal): Boolean;
external 'InitializeAcl@advapi32.dll stdcall';
function AppendAce(pAcl: cardinal; dwAceRevision: cardinal; dwStartingAceIndex: cardinal; pAceList: cardinal; nAceListLength: cardinal): Boolean;
external 'AddAce@advapi32.dll stdcall';
function AppendAllowAce(pAcl: cardinal; dwAceRevision: cardinal; AceFlags: cardinal; AccessMask: cardinal; pSid: cardinal): Boolean;
external 'AddAccessAllowedAceEx@advapi32.dll stdcall';
function ReadReparseData(hDevice: Integer; dwIoControlCode: cardinal; lpInBuffer: cardinal; nInBufferSize: cardinal; lpOutBuffer: String; nOutBufferSize: cardinal;
                         var lpBytesReturned: cardinal; lpOverlapped: cardinal): Boolean;
external 'DeviceIoControl@kernel32.dll stdcall';
function MakeFileRelative(var FileHandle: Integer; DesiredAccess: cardinal; var ObjectAttributes: HM_OBJECT_ATTRIBUTES; var IoStatusBlock: HM_IO_STATUS_BLOCK;
                          AllocationSize: cardinal; FileAttributes: cardinal; ShareAccess: cardinal; CreateDisposition: cardinal; CreateOptions: cardinal;
                          EaBuffer: cardinal; EaLength: cardinal): cardinal;
external 'NtCreateFile@ntdll.dll stdcall';
procedure CopyTextTo(Destination: cardinal; Source: String; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
procedure WriteNameRecord(Destination: cardinal; var Source: HM_UNICODE_STRING; Length: cardinal);
external 'RtlMoveMemory@kernel32.dll stdcall';
function VolumeOfHeldFolder(hFile: Integer; lpVolumeNameBuffer: cardinal; nVolumeNameSize: cardinal; var lpVolumeSerialNumber: cardinal; var lpMaximumComponentLength: cardinal;
                            var lpFileSystemFlags: cardinal; lpFileSystemNameBuffer: cardinal; nFileSystemNameSize: cardinal): Boolean;
external 'GetVolumeInformationByHandleW@kernel32.dll stdcall';
function KindOfDrive(lpRootPathName: String): cardinal;
external 'GetDriveTypeW@kernel32.dll stdcall';
function LookupNameOfSid(lpSystemName: cardinal; Sid: cardinal; Name: String; var cchName: cardinal; ReferencedDomainName: String; var cchReferencedDomainName: cardinal;
                         var peUse: cardinal): Boolean;
external 'LookupAccountSidW@advapi32.dll stdcall';
// What a held folder is now, read through its handle, which names that folder
// whatever has been done to its path since: GetFileInformationByHandle, for its
// attributes (HeldIsLink), and GetFinalPathNameByHandleW, for where it is
// (FinalPathOfHeld). Named apart from the API for the reason
// SetProcessEnvironmentVariable is.
function ReadHeldInformation(hFile: Integer; var lpFileInformation: HM_FILE_INFORMATION): Boolean;
external 'GetFileInformationByHandle@kernel32.dll stdcall';
function FinalPathOfHandle(hFile: Integer; lpszFilePath: String; cchFilePath: cardinal; dwFlags: cardinal): cardinal;
external 'GetFinalPathNameByHandleW@kernel32.dll stdcall';
// Work item 78's R2 and R3: RegCreateKeyExW, RegSetValueExW and RegCloseKey, for the
// volatile key that keeps setup from installing again before Windows restarts
// (RecordRestartNeeded) - Inno Setup's own registry functions make no volatile key - and
// SetFileInformationByHandle, to remove an empty folder through the handle that was
// read as that folder (RemoveHeldFolder). Named apart from the API for the reason
// SetProcessEnvironmentVariable is.
function MakeVolatileKey(hKey: cardinal; lpSubKey: String; Reserved: cardinal; lpClass: cardinal; dwOptions: cardinal; samDesired: cardinal; lpSecurityAttributes: cardinal;
                         var phkResult: cardinal; var lpdwDisposition: cardinal): Integer;
external 'RegCreateKeyExW@advapi32.dll stdcall';
function WriteKeyText(hKey: cardinal; lpValueName: String; Reserved: cardinal; dwType: cardinal; lpData: String; cbData: cardinal): Integer;
external 'RegSetValueExW@advapi32.dll stdcall';
function CloseKey(hKey: cardinal): Integer;
external 'RegCloseKey@advapi32.dll stdcall';
function SetHeldInformation(hFile: Integer; FileInformationClass: cardinal; var lpFileInformation: cardinal; dwBufferSize: cardinal): Boolean;
external 'SetFileInformationByHandle@kernel32.dll stdcall';
// GetVolumePathNamesForVolumeNameW, for whether a volume a data folder's link leads to
// the root of has a drive letter, whose root /GrantServiceAccess refuses
// (DataLinkLeadsWhereAllowed).
function VolumePathNames(lpszVolumeName: String; lpszVolumePathNames: String; cchBufferLength: cardinal; var lpcchReturnLength: cardinal): Boolean;
external 'GetVolumePathNamesForVolumeNameW@kernel32.dll stdcall';

// The groups that hold every user of the computer, or programs in an app container,
// as SIDs - Everyone, Authenticated Users, Users and the rest of ServiceAccount.cpp's
// BroadSids_, every one of them, which build/check-installer-scripts.py holds this list
// to (check 5 of R1 to R4, 27 October 2026: eight of them were left out, and an entry
// of a folder's own for one was kept as a named account's) - with the name each is
// given in what setup says; This Organization, Local account, the identity an
// authentication authority asserted and ALL SERVICES since check 6 of R1 to R4 (28 October
// 2026), in BroadSids_ too. A domain's Domain Users and Domain Guests (S-1-5-21-...-513 and
// -514) are two of them too (IsBroadSid). The first three are those EveryUserMayRead asks
// for. Empty past the last.
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
      10: begin Result := 'S-1-2-1'; Name := 'CONSOLE LOGON'; end;
      11: begin Result := 'S-1-5-2'; Name := 'NETWORK'; end;
      12: begin Result := 'S-1-5-3'; Name := 'BATCH'; end;
      13: begin Result := 'S-1-5-6'; Name := 'SERVICE'; end;
      14: begin Result := 'S-1-5-7'; Name := 'ANONYMOUS LOGON'; end;
      15: begin Result := 'S-1-15-2-1'; Name := 'ALL APPLICATION PACKAGES'; end;
      16: begin Result := 'S-1-15-2-2'; Name := 'ALL RESTRICTED APPLICATION PACKAGES'; end;
      17: begin Result := 'S-1-5-15'; Name := 'This Organization'; end;
      18: begin Result := 'S-1-5-113'; Name := 'Local account'; end;
      19: begin Result := 'S-1-18-1'; Name := 'Authentication authority asserted identity'; end;
      20: begin Result := 'S-1-5-80-0'; Name := 'ALL SERVICES'; end;
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
// run. A warning, never a refusal: setup makes the program folder with permissions
// of its own, or hardens one that is there, Bin and ControlPanel with it, before any
// file is copied (PrepareProgramFolders), and hMailServer.exe /ProtectFolders
// hardens them again after the files (docs/ServiceAccount.md); a folder made for the
// purpose off a drive's root is a choice an administrator may make. In a box when
// Show is set and the installation is attended - the folder page - and in the setup
// log always; once for a folder.
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
             'Setup takes that right away from the program folder, Bin and ControlPanel before it copies anything - it makes a new program folder with permissions of its own, ' +
             'and hardens one that is there - and hMailServer.exe /ProtectFolders does again after the files. A folder above the program folder that lets such an account rename ' +
             'it, or remove what is in it, setup does not install below, since nothing setup does to the program folder stops that: it says so when the installation starts. ' +
             'A folder made for programs, such as the default ' + ExpandConstant('{commonpf}') + '\hMailServer, is the safer place.';
   Log('The program folder: ' + szText);
   if Show and not WizardSilent() then
      SuppressibleMsgBox(szText, mbInformation, MB_OK, IDOK);
end;

// Whether Everyone, Authenticated Users or Users may read what is in a folder, by an
// allow entry of its list, pDacl, that applies to it (BroadGroupSid's first three):
// what ServiceAccount::Harden asks before it gives BUILTIN\Users Read & execute, so
// that a folder made in one only the administrators may open does not become every
// user's to read. The list is read through the handle that holds the folder
// (HardenHeldFolder). True when it has no list at all.
function EveryUserMayRead(pDacl: cardinal): Boolean;
var
   szSid: String;
   szName: String;
   pAce: cardinal;
   pBroad: cardinal;
   info: ACL_SIZE_INFO;
   head: ACE_START;
   index: cardinal;
   group: Integer;
begin
   Result := false;
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
end;

// A SID as text (ConvertSidToStringSidW), or '' when it cannot be had.
function TextOfSidAt(pSid: cardinal): String;
var
   pText: cardinal;
   textLength: Integer;
   szText: String;
begin
   Result := '';
   if pSid = 0 then
      Exit;
   pText := 0;
   if not SidToText(pSid, pText) then
      Exit;
   textLength := WideTextLength(pText);
   if textLength > 0 then
   begin
      SetLength(szText, textLength);
      CopyWideText(szText, pText, textLength * 2);
      Result := szText;
   end;
   FreeWindowsMemory(pText);
end;

// Whether the SID at pSid is the one szSid names.
function SidIs(pSid: cardinal; szSid: String): Boolean;
var
   pOther: cardinal;
begin
   Result := false;
   if pSid = 0 then
      Exit;
   pOther := 0;
   if SidOfText(szSid, pOther) then
   begin
      Result := SidsAreEqual(pSid, pOther);
      FreeWindowsMemory(pOther);
   end;
end;

// SYSTEM, the Administrators or TrustedInstaller - ServiceAccount.cpp's
// IsTrustedWriter_: the accounts a program folder, and a link on the way to it, may
// belong to and be changeable by.
function IsTrustedSid(pSid: cardinal): Boolean;
begin
   Result := SidIs(pSid, 'S-1-5-18') or SidIs(pSid, 'S-1-5-32-544') or SidIs(pSid, HM_TRUSTED_INSTALLER_SID);
end;

// The account a SID names, DOMAIN\name, as LookupAccountSidW gives it in the
// language of this Windows, or '' when it names none this computer knows.
function AccountNameOfSid(pSid: cardinal): String;
var
   szName: String;
   szDomain: String;
   nameLength: cardinal;
   domainLength: cardinal;
   use: cardinal;
begin
   Result := '';
   SetLength(szName, 256);
   SetLength(szDomain, 256);
   nameLength := 256;
   domainLength := 256;
   use := 0;
   if LookupNameOfSid(0, pSid, szName, nameLength, szDomain, domainLength, use) then
   begin
      Result := Copy(szName, 1, nameLength);
      if domainLength > 0 then
         Result := Copy(szDomain, 1, domainLength) + '\' + Result;
   end;
end;

// A SID in what setup says: the group's name where it is one of BroadGroupSid's, or
// the account's name where this computer knows it, then the SID, which no language
// of Windows changes.
function SidNameAt(pSid: cardinal): String;
var
   szSid: String;
   szName: String;
   group: Integer;
begin
   Result := TextOfSidAt(pSid);
   if Result = '' then
      Result := 'an account whose SID could not be read'
   else
   begin
      szName := AccountNameOfSid(pSid);
      if szName <> '' then
         Result := szName + ' (' + Result + ')';
   end;
   group := 0;
   szSid := BroadGroupSid(group, szName);
   while szSid <> '' do
   begin
      if SidIs(pSid, szSid) then
      begin
         Result := szName + ' (' + szSid + ')';
         szSid := '';
      end
      else
      begin
         group := group + 1;
         szSid := BroadGroupSid(group, szName);
      end;
   end;
end;

// Whether the SID at pSid is one of the groups that hold every user (BroadGroupSid), or a
// domain's Domain Users or Domain Guests, S-1-5-21-...-513 or -514, as ServiceAccount.cpp's
// IsBroad_ counts them.
function IsBroadSid(pSid: cardinal): Boolean;
var
   szSid: String;
   szName: String;
   group: Integer;
begin
   Result := false;
   group := 0;
   szSid := BroadGroupSid(group, szName);
   while (not Result) and (szSid <> '') do
   begin
      if SidIs(pSid, szSid) then
         Result := true;
      group := group + 1;
      szSid := BroadGroupSid(group, szName);
   end;
   if not Result then
   begin
      szSid := TextOfSidAt(pSid);
      Result := (Length(szSid) > 13) and (CompareText(Copy(szSid, 1, 9), 'S-1-5-21-') = 0) and
                ((Copy(szSid, Length(szSid) - 3, 4) = '-513') or (Copy(szSid, Length(szSid) - 3, 4) = '-514'));
   end;
end;

// Whether a list of permissions lets an account other than SYSTEM, the Administrators
// or TrustedInstaller do any of Rights to what it is on, by an allow entry that
// applies to it - ProtectedFile.cpp's OthersMay_: OWNER RIGHTS counts as the owner,
// which OwnerTrusted says is one of the three. Who is that account, or what could not
// be read. No list at all lets everyone; an allow entry of a form a file system does
// not use is not read, and so not trusted either. Deny entries are not weighed: a
// deny only ever takes away, and this answers who may.
function AclLetsOthers(pDacl: cardinal; Rights: cardinal; OwnerTrusted: Boolean; var Who: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
   aceType: cardinal;
   aceFlags: cardinal;
begin
   Result := false;
   Who := '';
   if pDacl = 0 then
   begin
      Result := true;
      Who := 'everyone (it has no list of permissions at all)';
      Exit;
   end;
   if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      Result := true;
      Who := 'an account in a list of permissions that could not be read';
      Exit;
   end;

   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         aceType := head.TypeFlagsSize and $FF;
         aceFlags := (head.TypeFlagsSize shr 8) and $FF;
         if (aceFlags and HM_INHERIT_ONLY) = 0 then
         begin
            if aceType = HM_ACCESS_ALLOWED then
            begin
               if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) then
               begin
                  if not (OwnerTrusted and SidIs(pAce + 8, 'S-1-3-4')) then
                  begin
                     Result := true;
                     Who := SidNameAt(pAce + 8);
                  end;
               end;
            end
            else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then
            begin
               Result := true;
               Who := 'an account in an entry of a form setup does not read';
            end;
         end;
      end
      else
      begin
         Result := true;
         Who := 'an account in an entry that could not be read';
      end;
      index := index + 1;
   end;
end;

// Whether only SYSTEM, the Administrators and TrustedInstaller control a link - a
// junction, a symbolic link, a mount point - held open as itself (hLink), and the
// folder it is in (hParent): ProtectedFile.cpp's OnlyAdministratorsControl, read
// through the handles, which name what was looked at whatever is done to the path
// since. On the link: its owner, and every right that changes it, what it leads to
// or who may (HM_FOLDER_WRITE_RIGHTS: a link's data and attributes are how it is
// turned elsewhere, as ProtectedFile.cpp says). On its folder: its owner, and removing a
// name from it or taking its permissions (HM_REMOVE_FROM_FOLDER_RIGHTS) - not asked when
// hParent is -1, for a link in the program folder itself, which setup hardens or holds
// (CheckWhatElseIsInside). Why says what else may, or what could not be read.
function LinkIsAdministratorsAlone(hLink: Integer; hParent: Integer; var Why: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   szWho: String;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(hLink, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'its owner and permissions could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if not IsTrustedSid(pOwner) then
      Why := 'it is owned by ' + SidNameAt(pOwner) + ', who may change it whatever its permissions say'
   else if AclLetsOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szWho) then
      Why := szWho + ' may change it'
   else
      Result := true;
   FreeWindowsMemory(pDescriptor);
   if not Result then
      Exit;
   if hParent = -1 then
      Exit;

   Result := false;
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(hParent, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
      Why := 'the permissions of the folder it is in could not be read (Windows error ' + IntToStr(readResult) + ')'
   else if not IsTrustedSid(pOwner) then
      Why := 'the folder it is in is owned by ' + SidNameAt(pOwner) + ', who may remove it from there and put another in its place'
   else if AclLetsOthers(pDacl, HM_REMOVE_FROM_FOLDER_RIGHTS, true, szWho) then
      Why := szWho + ' may remove it from the folder it is in and put another in its place'
   else
      Result := true;
   if readResult = 0 then
      FreeWindowsMemory(pDescriptor);
end;

// Where a link held open as itself says it leads (FSCTL_GET_REPARSE_POINT), as a
// full path: C:\... or \\?\Volume{...}\..., a relative symbolic link's taken from
// the folder the link is in - ProtectedFile.cpp's LinkTarget_. Not the folder an open
// through it ends at: that open follows every link after this one too, and each of
// those is asked about in its turn. A link that leads to a share, or a reparse point
// of another kind, is not gone through: false, with Why.
function LinkTarget(hLink: Integer; Link: String; var Target: String; var Why: String): Boolean;
var
   buffer: String;
   returned: cardinal;
   tagLow: Integer;
   tagHigh: Integer;
   offset: Integer;
   size: Integer;
   start: Integer;
   relative: Boolean;
   bytes: Integer;
   szName: String;
begin
   Result := false;
   Target := '';
   Why := '';
   // MAXIMUM_REPARSE_DATA_BUFFER_SIZE, 16 KB, as wide characters.
   SetLength(buffer, 8192);
   returned := 0;
   if not ReadReparseData(hLink, HM_GET_REPARSE_POINT, 0, 0, buffer, 16384, returned, 0) then
   begin
      Why := 'where it leads could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   if returned < 20 then
   begin
      Why := 'where it leads could not be read';
      Exit;
   end;

   // REPARSE_DATA_BUFFER, read a wide character - two bytes - at a time: the tag,
   // its data's length and a reserved word, then the offset and length of the
   // substitute name and of the name shown, in bytes, from the path buffer - which
   // starts after those for a mount point, and after a symbolic link's flags too.
   tagLow := Ord(buffer[1]);
   tagHigh := Ord(buffer[2]);
   offset := Ord(buffer[5]);
   size := Ord(buffer[6]);
   relative := false;
   if (tagHigh = $A000) and (tagLow = $0003) then
      start := 9
   else if (tagHigh = $A000) and (tagLow = $000C) then
   begin
      start := 11;
      relative := (Ord(buffer[9]) and 1) <> 0;
   end
   else
   begin
      Why := 'it is a reparse point of a kind setup does not go through (tag ' + IntToStr(tagHigh) + ':' + IntToStr(tagLow) + ')';
      Exit;
   end;
   bytes := returned;
   if ((offset div 2) + (size div 2) + start - 1 > (bytes div 2)) or (size = 0) then
   begin
      Why := 'where it leads could not be read';
      Exit;
   end;
   szName := Copy(buffer, start + (offset div 2), size div 2);

   if relative then
      Target := ExpandFileName(AddBackslash(ExtractFileDir(Link)) + szName)
   else if CompareText(Copy(szName, 1, 8), '\??\UNC\') = 0 then
   begin
      Why := 'it leads to \\' + Copy(szName, 9, Length(szName)) + ', a share, which setup does not install through';
      Exit;
   end
   else if Copy(szName, 1, 4) = '\??\' then
   begin
      Target := Copy(szName, 5, Length(szName));
      if CompareText(Copy(Target, 1, 7), 'Volume{') = 0 then
         Target := '\\?\' + Target;
   end
   else
   begin
      Why := 'it leads to ' + szName + ', which is not a folder setup can name';
      Exit;
   end;
   Target := RemoveBackslashUnlessRoot(Target);
   if Target = '' then
   begin
      Why := 'where it leads could not be read';
      Exit;
   end;
   if Copy(Target, 1, 4) = '\\?\' then
      if Target[Length(Target)] = '}' then
         Target := Target + '\';
   Result := true;
end;

// A full path cut into the root of its volume - C:\, or \\?\Volume{...}\ - and the
// rest. A share's path, or one that is not full, is not: false, with Why.
function SplitAtRoot(Path: String; var Root: String; var Rest: String; var Why: String): Boolean;
var
   close: Integer;
   drive: Boolean;
begin
   Result := false;
   Root := '';
   Rest := '';
   Why := '';
   // Each character read only where the path is long enough: nested, not joined
   // with and, so that nothing depends on how a condition is evaluated.
   drive := false;
   if Length(Path) >= 2 then
      drive := Path[2] = ':';
   if drive then
   begin
      Root := Copy(Path, 1, 2) + '\';
      Rest := Copy(Path, 4, Length(Path));
      if Length(Path) >= 3 then
         if Path[3] <> '\' then
         begin
            Why := Path + ' is not a full path';
            Exit;
         end;
      Result := true;
   end
   else if CompareText(Copy(Path, 1, 11), '\\?\Volume{') = 0 then
   begin
      close := Pos('}', Path);
      if close = 0 then
      begin
         Why := Path + ' names no volume';
         Exit;
      end;
      Root := Copy(Path, 1, close) + '\';
      Rest := Copy(Path, close + 2, Length(Path));
      Result := true;
   end
   else if Copy(Path, 1, 2) = '\\' then
      Why := Path + ' is on a share of another computer, whose permissions that computer sets, and setup does not install there'
   else
      Why := Path + ' is not a full path';
end;

// Whether a path is the root of a volume - C:\, or \\?\Volume{...}\, where a mount point
// or a link to a volume's root leads - whose permissions are the volume's: setup does
// not change them (PrepareProgramFolders) nor hold its owner to the rule of the three
// (CheckProgramFolders), and makes or hardens Bin and ControlPanel there. Inno Setup's
// RemoveBackslashUnlessRoot takes the last backslash off the second, as off the root
// of a share, and the path then names the volume's device rather than its root folder;
// HoldFolderChain keeps it.
function IsVolumeRoot(Path: String): Boolean;
begin
   Result := Length(Path) <= 3;
   if not Result then
      if CompareText(Copy(Path, 1, 11), '\\?\Volume{') = 0 then
         Result := Copy(Path, Length(Path) - 1, 2) = '}\';
end;

// A folder held open as itself (HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE,
// HM_HOLD_FOLDER_FLAGS) and kept in g_aHeldFolders until DeinitializeSetup lets
// every one go: opened without following a link, for listing, its attributes and its
// permissions - a handle that asks for the attributes alone takes no part in
// Windows' check of sharing, and its refusal to share deleting would bind nobody
// (ProtectedFile.cpp's HeldFolder holds a folder the same way) - shared for reading
// and writing, which the files setup copies in need, but not for deleting: until it
// is let go nobody can rename it, remove it or put a link in its place. -1 when it
// cannot be opened.
function HoldForSetup(Folder: String): Integer;
begin
   Result := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   if Result <> -1 then
   begin
      SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1);
      g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result;
   end;
end;

procedure ReleaseHeldFolders();
var
   index: Integer;
begin
   for index := GetArrayLength(g_aHeldFolders) - 1 downto 0 do
      ReleaseFolder(g_aHeldFolders[index]);
   SetArrayLength(g_aHeldFolders, 0);
end;

// One folder setup holds let go, and forgotten, so that ReleaseHeldFolders does not
// let it go again (RemakeEmptyFolder).
procedure LetGoOfHeld(Handle: Integer);
var
   index: Integer;
   last: Integer;
begin
   for index := GetArrayLength(g_aHeldFolders) - 1 downto 0 do
      if g_aHeldFolders[index] = Handle then
      begin
         ReleaseFolder(Handle);
         last := GetArrayLength(g_aHeldFolders) - 1;
         g_aHeldFolders[index] := g_aHeldFolders[last];
         SetArrayLength(g_aHeldFolders, last);
      end;
end;

// Whether a folder holds nothing at all; false when what it holds cannot be read.
function FolderHoldsNothing(Folder: String): Boolean;
var
   rec: TFindRec;
begin
   Result := false;
   if FindFirst(AddBackslash(Folder) + '*', rec) then
   begin
      try
         Result := true;
         repeat
            if (rec.Name <> '.') and (rec.Name <> '..') then
               Result := false;
         until (not Result) or (not FindNext(rec));
      finally
         FindClose(rec);
      end;
   end;
end;

// Whether all a folder holds, if anything, is empty folders: what setup makes again,
// removes before the files (CheckWhatIsInside, CheckWhatElseIsInside) or leaves for
// Inno Setup to make, so that nothing a handle opened in it before setup hardened it
// is left there for the installation to use (CheckProgramFolders). False when it holds
// a file, a link or a folder that holds anything, and when what it holds cannot be read.
function HoldsOnlyEmptyFolders(Folder: String): Boolean;
var
   rec: TFindRec;
begin
   Result := false;
   if FindFirst(AddBackslash(Folder) + '*', rec) then
   begin
      try
         Result := true;
         repeat
            if (rec.Name <> '.') and (rec.Name <> '..') then
            begin
               if ((rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) or ((rec.Attributes and HM_REPARSE_POINT) <> 0) then
                  Result := false
               else if not FolderHoldsNothing(AddBackslash(Folder) + rec.Name) then
                  Result := false;
            end;
         until (not Result) or (not FindNext(rec));
      finally
         FindClose(rec);
      end;
   end;
end;

// Where a held folder is now (GetFinalPathNameByHandleW), as \\?\Volume{...}\..., read
// through the handle that holds it, or '' when it cannot be read.
function FinalPathOfHeld(HeldHandle: Integer): String;
var
   buffer: String;
   size: cardinal;
   capacity: cardinal;
begin
   Result := '';
   capacity := 1024;
   SetLength(buffer, capacity);
   size := FinalPathOfHandle(HeldHandle, buffer, capacity, HM_VOLUME_NAME_GUID);
   if size >= capacity then
   begin
      capacity := size + 1;
      SetLength(buffer, capacity);
      size := FinalPathOfHandle(HeldHandle, buffer, capacity, HM_VOLUME_NAME_GUID);
   end;
   if (size > 0) and (size < capacity) then
      Result := Copy(buffer, 1, size);
end;

// Where a folder named by its path is, its links followed, as \\?\Volume{...}\...
// (FinalPathOfHeld), or '' when it cannot be opened: for a folder setup asks whether
// another is, holds or is in (DataLinkLeadsWhereAllowed), whatever letter or mount
// point names either.
function FinalPathOf(Path: String): String;
var
   handle: Integer;
begin
   Result := '';
   handle := HoldFolder(Path, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_OPEN_FOLLOWING_FLAGS, 0);
   if handle = -1 then
      Exit;
   Result := FinalPathOfHeld(handle);
   ReleaseFolder(handle);
end;

// Whether a held folder is a link now - a junction, a symbolic link, a mount point,
// any reparse point - read through the handle that holds it, which was opened on the
// folder itself and not through a link: an empty folder another account may write
// can be made one while it is held, since the hold shares writing. True too when its
// attributes cannot be read, Known then false.
function HeldIsLink(HeldHandle: Integer; var Known: Boolean): Boolean;
var
   information: HM_FILE_INFORMATION;
begin
   Result := true;
   Known := ReadHeldInformation(HeldHandle, information);
   if Known then
      Result := (information.dwFileAttributes and HM_REPARSE_POINT) <> 0;
end;

// Whether the file or folder at the path Item - opened by its path, as Inno Setup
// copied the files, through any link on the way - is the one of its name in the folder
// setup holds by FolderHandle, read through the two handles (FinalPathOfHeld): a link
// put on the way, or one an old handle turned elsewhere, leads the path to another
// folder than the one setup prepared (FoldersStillAsPrepared). False when either cannot
// be read.
function ItemIsIn(Item: String; FolderHandle: Integer): Boolean;
var
   handle: Integer;
   szItem: String;
   szFolder: String;
begin
   Result := false;
   handle := HoldFolder(Item, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   if handle = -1 then
      Exit;
   szItem := FinalPathOfHeld(handle);
   ReleaseFolder(handle);
   szFolder := FinalPathOfHeld(FolderHandle);
   if (szItem <> '') and (szFolder <> '') then
      Result := CompareText(szItem, AddBackslash(szFolder) + ExtractFileName(Item)) = 0;
end;

// Whether the folder held by Made is the one named Name in the folder held by Parent,
// read through the two handles: where a link was put on the way since the walk, a
// folder made by its path is made where the link leads, and its path then is not the
// held folder's with its name. Where says where it is when it is not.
function MadeWhereNamed(Parent: Integer; Made: Integer; Name: String; var Where: String): Boolean;
var
   szParent: String;
   szMade: String;
begin
   Result := false;
   szParent := FinalPathOfHeld(Parent);
   szMade := FinalPathOfHeld(Made);
   if (szParent = '') or (szMade = '') then
      Where := 'where it was made could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')'
   else if CompareText(szMade, AddBackslash(szParent) + Name) <> 0 then
      Where := 'was made at ' + szMade + ', not in ' + szParent
   else
      Result := true;
end;

// Work item 78's R2 and R3: an empty folder removed through a handle opened on it as
// itself (HM_REMOVE_FOLDER_ACCESS, every sharing, a link not followed), and only once
// that handle is found to be no link and the folder of its name in the one whose final
// path is ParentFinal - read before, through the handle that holds that one: by its
// path, through a link put on the way since it was listed, setup would remove what
// another account chose (check 4 of R1 to R4, 27 October 2026). Removed with POSIX
// semantics (FileDispositionInfoEx), so that its name is gone at once even while
// another program holds it open and Inno Setup can make it again, or the classic way
// where the volume does not take that; one that holds something Windows does not
// remove. True when it is gone; false, with Why, otherwise.
function RemoveHeldFolder(Folder: String; ParentFinal: String; var Why: String): Boolean;
var
   handle: Integer;
   info: cardinal;
   bKnown: Boolean;
   szFinal: String;
begin
   Result := false;
   Why := '';
   handle := HoldFolder(Folder, HM_REMOVE_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   if handle = -1 then
   begin
      Why := 'it could not be opened to be removed (Windows error ' + IntToStr(DLLGetLastError()) + '): another program may hold it open without sharing its removal';
      Exit;
   end;
   szFinal := FinalPathOfHeld(handle);
   if HeldIsLink(handle, bKnown) then
      Why := 'it is a junction, a symbolic link or a mount point now, or what it is could not be read'
   else if (szFinal = '') or (ParentFinal = '') or (CompareText(szFinal, AddBackslash(ParentFinal) + ExtractFileName(Folder)) <> 0) then
      Why := 'its path does not lead to the folder of its name in the folder setup looked at now (it leads to ' + szFinal + ')'
   else
   begin
      info := HM_DISPOSITION_DELETE_POSIX;
      Result := SetHeldInformation(handle, HM_FILE_DISPOSITION_INFO_EX, info, 4);
      if not Result then
      begin
         info := 1;
         Result := SetHeldInformation(handle, HM_FILE_DISPOSITION_INFO, info, 1);
      end;
      if not Result then
         Why := 'Windows did not remove it (Windows error ' + IntToStr(DLLGetLastError()) + ')';
   end;
   ReleaseFolder(handle);
end;

// Work item 78's R2: a file setup keeps in a folder it hardened in place - the program
// folder, Bin or ControlPanel - from just after the hardening until its files are in
// (HM_PLACEHOLDER_NAME, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE): a folder that holds
// something cannot be made a junction, whatever handle another account opened on it
// before setup hardened it, and the other things in it may be removed before the files
// - an empty Bin made again in the program folder, an empty folder removed at the last
// look - or through such a handle. Made where the folder held by HeldHandle is, read
// through the two handles (MadeWhereNamed), or refused: the folder was made a link
// first. Held in g_aPlaceholders and gone the moment setup lets it go
// (ReleasePlaceholders), or setup ends, however it ends. True when it is in place;
// false, with Why, otherwise.
function MakePlaceholder(Folder: String; HeldHandle: Integer; var Why: String): Boolean;
var
   handle: Integer;
   szWhere: String;
begin
   Result := false;
   Why := '';
   handle := HoldFolder(AddBackslash(Folder) + HM_PLACEHOLDER_NAME, HM_PLACEHOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_CREATE_NEW, HM_PLACEHOLDER_FLAGS, 0);
   if handle = -1 then
   begin
      Why := AddBackslash(Folder) + HM_PLACEHOLDER_NAME + ', which setup keeps in ' + Folder + ' until the files are in, could not be made (Windows error ' +
             IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   SetArrayLength(g_aPlaceholders, GetArrayLength(g_aPlaceholders) + 1);
   g_aPlaceholders[GetArrayLength(g_aPlaceholders) - 1] := handle;
   if not MadeWhereNamed(HeldHandle, handle, HM_PLACEHOLDER_NAME, szWhere) then
   begin
      g_bFoldersChanged := true;
      Why := 'the file setup keeps in ' + Folder + ' until the files are in ' + szWhere + ': ' + Folder + ' was made a link while setup was preparing the installation';
      Exit;
   end;
   Log('Setup keeps ' + AddBackslash(Folder) + HM_PLACEHOLDER_NAME + ' in ' + Folder + ', which it hardened in place, until the files are in: a folder that holds something ' +
       'cannot be made a junction, whatever a handle opened on it before setup hardened it may do.');
   Result := true;
end;

// The files MakePlaceholder keeps, let go - and so gone - at ssPostInstall, once the
// folders are read again after the files (FoldersStillAsPrepared), and in
// DeinitializeSetup.
procedure ReleasePlaceholders();
var
   index: Integer;
begin
   for index := GetArrayLength(g_aPlaceholders) - 1 downto 0 do
      ReleaseFolder(g_aPlaceholders[index]);
   SetArrayLength(g_aPlaceholders, 0);
end;

// What an administrator does with a link setup does not install through: makes it
// theirs alone, as the link setup takes, or installs elsewhere.
function LinkRemedy(Link: String): String;
begin
   Result := 'An administrator who made it, and means it to be there, makes it theirs alone - icacls "' + Link + '" /L /setowner *S-1-5-32-544, then icacls "' + Link +
             '" /L /inheritance:r /grant:r *S-1-5-18:F *S-1-5-32-544:F *S-1-5-32-545:RX - restarts Windows, since a program that opened the link while another account could ' +
             'change it keeps that right until it closes it, and runs setup again; otherwise it is removed, or hMailServer installed into a folder that is not reached through it';
end;

// The SID in what SidNameAt says of an account - the text between its last
// parentheses when it is one - or '' when there is none.
function SidInName(Who: String): String;
var
   index: Integer;
   close: Integer;
begin
   Result := '';
   close := 0;
   for index := Length(Who) downto 1 do
   begin
      if (close = 0) and (Who[index] = ')') then
         close := index
      else if (close > 0) and (Result = '') and (Who[index] = '(') then
         Result := Copy(Who, index + 1, close - index - 1);
   end;
   if Copy(Result, 1, 2) <> 'S-' then
      Result := '';
end;

// Whether Folder is the program folder its walk ended at, or a folder above it
// (g_szFolderAboveChecked): one Bin's and ControlPanel's walks pass on the way, which
// the program folder's held to the rule already.
function IsAtOrAboveChecked(Folder: String): Boolean;
var
   szFolder: String;
begin
   Result := false;
   if g_szFolderAboveChecked = '' then
      Exit;
   szFolder := AddBackslash(Folder);
   Result := CompareText(Copy(AddBackslash(g_szFolderAboveChecked), 1, Length(szFolder)), szFolder) = 0;
end;

// Work item 78's R3, for a folder on the way that is not a link: whether a folder
// above the program folder, Bin or ControlPanel - the root of its volume included -
// held open as itself (Handle), is SYSTEM's, the Administrators' or TrustedInstaller's
// and lets no other account rename it, remove what is in it or change who may
// (HM_REPLACE_RIGHTS, an allow entry that applies to it): the rule the live update
// holds every folder above the program folder to (AdminUpdateChecks' CheckAncestors_),
// read through the handle that holds it. Any other account could, once setup has let
// it go, put a folder of its own, and programs of its own, in the program folder's
// place - one a link setup refuses or not - and the program folder's own permissions
// cannot stop it. Why names the folder, the account and what to do.
function FolderAboveIsAdministratorsAlone(Handle: Integer; Folder: String; var Why: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   szWho: String;
   szSid: String;
   szRemedy: String;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'the owner and permissions of ' + Folder + ', a folder on the way to it, could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if not IsTrustedSid(pOwner) then
      Why := Folder + ', a folder on the way to it, is owned by ' + SidNameAt(pOwner) + ', who may change its permissions whatever they say, and so rename it or remove what is ' +
             'in it, and put a folder of its own in the program folder''s place once setup has finished. An administrator who means it to be there gives it to the Administrators - ' +
             'icacls "' + Folder + '" /setowner *S-1-5-32-544 - restarts Windows, since a program that account started may still hold it open with the right to change ' +
             'it, and runs setup again; otherwise hMailServer is installed into a folder such as the default ' +
             ExpandConstant('{commonpf}') + '\hMailServer'
   else if AclLetsOthers(pDacl, HM_REPLACE_RIGHTS, true, szWho) then
   begin
      szSid := SidInName(szWho);
      if szSid <> '' then
         szRemedy := 'icacls "' + Folder + '" /inheritance:d, then icacls "' + Folder + '" /remove:g *' + szSid
      else
         szRemedy := 'icacls "' + Folder + '" /inheritance:d, then icacls "' + Folder + '" /remove:g with that account''s SID';
      Why := Folder + ', a folder on the way to it, lets ' + szWho + ' rename it, remove what is in it or change who may, so that account could put a folder of its own, ' +
             'and programs of its own, in the program folder''s place once setup has finished, whatever the program folder''s own permissions say; the live update refuses such an ' +
             'installation too. An administrator who means that account to have no such right takes it away - ' + szRemedy + ' - restarts Windows, since a program that account started may still hold it open with that right, and runs ' +
             'setup again; otherwise hMailServer is ' +
             'installed into a folder such as the default ' + ExpandConstant('{commonpf}') + '\hMailServer';
   end
   else
      Result := true;
   FreeWindowsMemory(pDescriptor);
end;

// The path given held from the root of its volume down, every folder on it that is
// there opened as itself and held (HoldForSetup), each before the one below it is
// looked at - work item 78's links (R3). A folder on the way that is a link is gone
// through only when SYSTEM, the Administrators and TrustedInstaller alone control it
// and the folder it is in (LinkIsAdministratorsAlone): the path is then walked again
// from where the link says it leads (LinkTarget), with the rest of the path after it,
// so that every link there is asked about in its turn - ProtectedFile.cpp's
// HeldFolder::HoldThrough_ - to eight links deep. Real is the path the folders
// reached were found at, the folders not there yet included: no link in it. Held is
// the deepest folder of it that was found and held, with the handle that holds it
// (HeldHandle); Whole says whether that is the last, Real itself. A name found
// missing is not held, and somebody else may make it, or a link, the moment after:
// setup decides what is there by Whole and Held alone, never by looking at the path
// again, and makes what was missing below Held by each name relative to the handle of
// the folder above it, which refuses a name somebody else has taken and goes through no
// link (MakeFolders, MakeFolderIn). False, with Why - which names the
// folder and what to do - when a link is not gone through, a folder cannot be held
// or read, or the path is not one setup installs into: a share's, a network drive's,
// one whose volume cannot be named. A folder on the way that is not a link - the root
// of the volume included - is held to the rule of a folder above the program folder
// (FolderAboveIsAdministratorsAlone), but for those the program folder's walk held to
// it already (IsAtOrAboveChecked). Nothing is changed.
function HoldFolderChain(Path: String; Depth: Integer; var Real: String; var Held: String; var HeldHandle: Integer; var Whole: Boolean; var Why: String): Boolean;
var
   szRoot: String;
   szRest: String;
   szName: String;
   szNext: String;
   szCurrent: String;
   szTarget: String;
   szWhy: String;
   separator: Integer;
   attributes: cardinal;
   lastError: Integer;
   handle: Integer;
   parent: Integer;
   bDone: Boolean;
begin
   Result := false;
   Real := '';
   Held := '';
   HeldHandle := -1;
   Whole := false;
   Why := '';
   if Depth >= 8 then
   begin
      Why := Path + ' is reached through more than eight links, and setup does not install through them';
      Exit;
   end;
   if not SplitAtRoot(Path, szRoot, szRest, Why) then
      Exit;
   if KindOfDrive(szRoot) = HM_DRIVE_REMOTE then
   begin
      Why := Path + ' is on a network drive, whose permissions another computer sets, and setup does not install there';
      Exit;
   end;

   parent := HoldForSetup(szRoot);
   if parent = -1 then
   begin
      Why := 'the root of ' + Path + ', ' + szRoot + ', could not be opened (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   if szRest <> '' then
      if not IsAtOrAboveChecked(szRoot) then
         if not FolderAboveIsAdministratorsAlone(parent, szRoot, szWhy) then
         begin
            Why := szWhy;
            Exit;
         end;

   szCurrent := szRoot;
   bDone := false;
   while not bDone do
   begin
      // The next name on the path; an empty one, of a doubled separator, passed over.
      separator := Pos('\', szRest);
      if separator = 0 then
      begin
         szName := szRest;
         szRest := '';
      end
      else
      begin
         szName := Copy(szRest, 1, separator - 1);
         szRest := Copy(szRest, separator + 1, Length(szRest));
      end;

      if szName = '' then
      begin
         if szRest = '' then
         begin
            // The root of the volume as it is, \\?\Volume{...}\ with its backslash
            // (IsVolumeRoot).
            if CompareText(szCurrent, szRoot) = 0 then
               Real := szRoot
            else
               Real := RemoveBackslashUnlessRoot(szCurrent);
            Held := Real;
            HeldHandle := parent;
            Whole := true;
            Result := true;
            bDone := true;
         end;
      end
      else
      begin
         szNext := AddBackslash(szCurrent) + szName;
         attributes := FolderAttributes(szNext);
         lastError := DLLGetLastError();
         if attributes = HM_INVALID_ATTRIBUTES then
         begin
            if (lastError = 2) or (lastError = 3) then
            begin
               // Not there, and neither is anything below it: the folders setup
               // makes, below the deepest one held.
               Real := szNext;
               if szRest <> '' then
                  Real := AddBackslash(szNext) + szRest;
               if CompareText(szCurrent, szRoot) = 0 then
                  Held := szRoot
               else
                  Held := RemoveBackslashUnlessRoot(szCurrent);
               HeldHandle := parent;
               Whole := false;
               Result := true;
            end
            else
               Why := szNext + ' could not be read (Windows error ' + IntToStr(lastError) + ')';
            bDone := true;
         end
         else
         begin
            handle := HoldForSetup(szNext);
            if handle = -1 then
            begin
               Why := szNext + ' could not be held open while setup looks at it (Windows error ' + IntToStr(DLLGetLastError()) + ')';
               bDone := true;
            end
            else
            begin
               // Read again now that it is held: the name is the held folder's until
               // it is let go.
               attributes := FolderAttributes(szNext);
               if (attributes = HM_INVALID_ATTRIBUTES) or ((attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then
               begin
                  Why := szNext + ' is not a folder';
                  bDone := true;
               end
               else if (attributes and HM_REPARSE_POINT) <> 0 then
               begin
                  if not LinkIsAdministratorsAlone(handle, parent, szWhy) then
                     Why := szNext + ' is a junction, a symbolic link or a mount point, and ' + szWhy + ', so setup does not install through it. ' + LinkRemedy(szNext)
                  else if not LinkTarget(handle, szNext, szTarget, szWhy) then
                     Why := szNext + ' is a junction, a symbolic link or a mount point, and ' + szWhy
                  else
                  begin
                     Log(szNext + ' is a link that only SYSTEM, the Administrators and TrustedInstaller control; it leads to ' + szTarget + ', which setup holds and looks at in its turn.');
                     if szRest <> '' then
                        szTarget := AddBackslash(szTarget) + szRest;
                     Result := HoldFolderChain(szTarget, Depth + 1, Real, Held, HeldHandle, Whole, Why);
                  end;
                  bDone := true;
               end
               else
               begin
                  // A folder with a name below it: one above the program folder, Bin
                  // or ControlPanel.
                  if szRest <> '' then
                     if not IsAtOrAboveChecked(szNext) then
                        if not FolderAboveIsAdministratorsAlone(handle, szNext, szWhy) then
                        begin
                           Why := szWhy;
                           bDone := true;
                        end;
                  if not bDone then
                  begin
                     parent := handle;
                     szCurrent := szNext;
                     if szRest = '' then
                     begin
                        Real := szNext;
                        Held := szNext;
                        HeldHandle := handle;
                        Whole := true;
                        Result := true;
                        bDone := true;
                     end;
                  end;
               end;
            end;
         end;
      end;
   end;
end;

// Whether every user could read a folder made in Folder: an allow entry for
// Everyone, Authenticated Users or Users (BroadGroupSid's first three) that reads
// (HM_FOLDER_READ_RIGHTS) and is handed down to folders - what such a folder would
// have inherited, and what ServiceAccount::Harden asks before it gives BUILTIN\Users
// Read & execute (EveryUserMayRead for a folder that is there). Read through the
// handle that holds the folder (HoldForSetup). True when it has no list at all;
// false when it cannot be read.
function EveryUserMayReadMadeIn(HeldHandle: Integer): Boolean;
var
   szSid: String;
   szName: String;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pOwner: cardinal;
   pAce: cardinal;
   info: ACL_SIZE_INFO;
   head: ACE_START;
   index: cardinal;
   group: Integer;
begin
   Result := false;
   pDescriptor := 0;
   pDacl := 0;
   pOwner := 0;
   if ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then
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
            if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and HM_CONTAINER_INHERIT) <> 0) and
               ((head.Mask and HM_FOLDER_READ_RIGHTS) <> 0) then
            begin
               group := 0;
               while (not Result) and (group < 3) do
               begin
                  szSid := BroadGroupSid(group, szName);
                  if SidIs(pAce + 8, szSid) then
                     Result := true;
                  group := group + 1;
               end;
            end;
         end;
         index := index + 1;
      end;
   end;

   FreeWindowsMemory(pDescriptor);
end;

// Work item 78's R1 and R3 (check 5 of R1 to R4, 27 October 2026): a folder made in the
// folder held by Parent, by its one name relative to that handle - NtCreateFile with
// OBJECT_ATTRIBUTES' RootDirectory, FILE_CREATE and FILE_DIRECTORY_FILE |
// FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT - with the security descriptor
// pDescriptor, and held as it is made as HoldForSetup holds a folder - shared for reading
// and writing, not for deleting - and kept in g_aHeldFolders. No path is gone by, so that
// no link put on the way since the walk is followed: a folder above made a junction since
// it was held refuses the creation (measured on 27 October 2026: NTSTATUS $C0000280, or
// $C00004BC from this code, nothing made where the junction led), where CreateDirectoryW by the path made
// the folder where the junction led and setup refused only after. The handle, or -1 with
// Status the NTSTATUS (HM_STATUS_NAME_COLLISION for a name somebody else has taken).
function MakeFolderIn(Parent: Integer; Name: String; pDescriptor: cardinal; var Status: cardinal): Integer;
var
   attributes: HM_OBJECT_ATTRIBUTES;
   nameRecord: HM_UNICODE_STRING;
   io: HM_IO_STATUS_BLOCK;
   pName: cardinal;
   bytes: cardinal;
   handle: Integer;
begin
   Result := -1;
   Status := HM_STATUS_NO_MEMORY;
   bytes := Length(Name) * 2;
   pName := AllocateWindowsMemory(HM_LPTR, 8 + bytes + 2);
   if pName = 0 then
      Exit;
   CopyTextTo(pName + 8, Name, bytes);
   nameRecord.Lengths := bytes or ((bytes + 2) shl 16);
   nameRecord.Buffer := pName + 8;
   WriteNameRecord(pName, nameRecord, 8);
   attributes.nLength := HM_OBJECT_ATTRIBUTES_SIZE;
   attributes.RootDirectory := Parent;
   attributes.ObjectName := pName;
   attributes.Attributes := HM_OBJ_CASE_INSENSITIVE;
   attributes.SecurityDescriptor := pDescriptor;
   attributes.SecurityQualityOfService := 0;
   io.Status := 0;
   io.Information := 0;
   handle := -1;
   Status := MakeFileRelative(handle, HM_MAKE_FOLDER_ACCESS, attributes, io, 0, 0, HM_SHARE_ALL_BUT_DELETE, HM_FILE_CREATE, HM_MAKE_FOLDER_OPTIONS, 0, 0);
   FreeWindowsMemory(pName);
   if Status <> 0 then
      Exit;
   Result := handle;
   SetArrayLength(g_aHeldFolders, GetArrayLength(g_aHeldFolders) + 1);
   g_aHeldFolders[GetArrayLength(g_aHeldFolders) - 1] := Result;
end;

// Work item 78's R1: the program folder of a new installation, and every folder above
// it that is not there yet - and Bin and ControlPanel when they are not there in a
// program folder that is - made by setup itself with permissions of their own from
// the moment each is made - a security descriptor given as it is made - rather
// than by Inno Setup, which made the folder with what the folder above hands down and
// left icacls to replace it afterwards: SYSTEM and the Administrators full control,
// BUILTIN\Users Read & execute where every user could read a folder made there
// (EveryUserMayReadMadeIn), handed down, nothing inherited, the Administrators the
// owner - what hMailServer.exe /ProtectFolders gives a program folder
// (ServiceAccount::Harden), by SID. Each is held as it is made (MakeFolderIn), and,
// once it is found where it was named (below), recorded in g_aFoldersMade,
// shallowest first, with where it was made and the handle that holds it - one found
// elsewhere is not recorded, and so never removed through the path that led it
// there (RemoveMadeFolder): section_dirs.iss gives the
// uninstaller each to remove when it is empty (FolderMadeBySetup, SetupMadeFolder,
// SetupMadeProgramFolder), and DeinitializeSetup removes those still empty when the
// installation did not reach its files. What is made is exactly the names of Folder
// below Held, the deepest folder the walk found and held (HoldFolderChain), and
// nothing is looked at again by its path: each is made by its name relative to the handle
// of the folder above it (MakeFolderIn), the first in Held through HeldHandle, so that
// a name that somebody else has taken since the walk - a folder or a link made in the
// meantime - is refused (HM_STATUS_NAME_COLLISION), and then setup does not install:
// false, with Why. A link put on the way since the walk is refused too, through the
// handles, and nothing is made where it leads: Held is read again through HeldHandle
// before anything is made, and refused when it is a link now (HeldIsLink) - an empty
// folder another account may write can be made one while it is held, since the hold
// shares writing - and one made a link after that refuses the creation relative to its
// handle; each folder made is held as it is made, and read through its handle where it
// is (MadeWhereNamed), refused unless it is in the folder held above it with its name.
// Once a folder is made in the one above, that one is no longer empty, and nothing can
// make it a link. Kind says
// what is made: 0 the program folder and the folders above it (g_aFoldersMade,
// g_bProgramFolderNew), 1 Bin and 2 ControlPanel in a program folder that is there
// (g_szBinMade, g_szControlPanelMade; section_dirs.iss gives each to the uninstaller
// with SetupMadeBin and SetupMadeControlPanel). MadeHandle holds the last folder made,
// Folder itself, when it is made.
function MakeFolders(Held: String; HeldHandle: Integer; Folder: String; Kind: Integer; var MadeHandle: Integer; var Why: String): Boolean;
var
   missing: array of String;
   szBase: String;
   szRest: String;
   szName: String;
   szPath: String;
   szDescriptor: String;
   szSaid: String;
   szWhere: String;
   pDescriptor: cardinal;
   status: cardinal;
   count: Integer;
   index: Integer;
   separator: Integer;
   handle: Integer;
   parent: Integer;
   bKnown: Boolean;
begin
   Result := false;
   Why := '';
   MadeHandle := -1;

   // The names below Held, shallowest first.
   szBase := AddBackslash(Held);
   if (Length(Folder) <= Length(szBase)) or (CompareText(Copy(Folder, 1, Length(szBase)), szBase) <> 0) then
   begin
      Why := Folder + ' is not below ' + Held + ', the folder setup found and holds';
      Exit;
   end;
   szRest := Copy(Folder, Length(szBase) + 1, Length(Folder));
   szPath := Held;
   count := 0;
   while szRest <> '' do
   begin
      separator := Pos('\', szRest);
      if separator = 0 then
      begin
         szName := szRest;
         szRest := '';
      end
      else
      begin
         szName := Copy(szRest, 1, separator - 1);
         szRest := Copy(szRest, separator + 1, Length(szRest));
      end;
      if szName <> '' then
      begin
         szPath := AddBackslash(szPath) + szName;
         SetArrayLength(missing, count + 1);
         missing[count] := szPath;
         count := count + 1;
      end;
   end;
   if count = 0 then
   begin
      Why := Folder + ' names no folder below ' + Held;
      Exit;
   end;

   szDescriptor := 'O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';
   szSaid := 'no entry for Users, as no user could read a folder made in ' + Held;
   if EveryUserMayReadMadeIn(HeldHandle) then
   begin
      szDescriptor := szDescriptor + '(A;OICI;0x1200a9;;;BU)';
      szSaid := 'Users Read & execute';
   end;
   pDescriptor := 0;
   if not SecurityDescriptorOfText(szDescriptor, 1, pDescriptor, 0) then
   begin
      Why := 'the permissions for ' + Folder + ' could not be made (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;

   Result := true;
   if HeldIsLink(HeldHandle, bKnown) then
   begin
      g_bFoldersChanged := true;
      if bKnown then
         Why := Held + ' was made a junction, a symbolic link or a mount point while setup was preparing the installation, so setup does not install there - run setup again to have it looked at'
      else
         Why := 'what ' + Held + ' is now could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Result := false;
   end;
   index := 0;
   parent := HeldHandle;
   while Result and (index < count) do
   begin
      status := 0;
      handle := MakeFolderIn(parent, ExtractFileName(missing[index]), pDescriptor, status);
      if handle <> -1 then
      begin
         if not MadeWhereNamed(parent, handle, ExtractFileName(missing[index]), szWhere) then
         begin
            g_bFoldersChanged := true;
            Why := missing[index] + ', just made, ' + szWhere + ': a folder on the way was made a link while setup was preparing the installation, so setup does not install there - run setup again to have it looked at';
            Log('The folder setup made for ' + missing[index] + ' is not removed by setup, which would remove it through the link that led it elsewhere: it stays, empty, with setup''s permissions.');
            Result := false;
         end
         else
         begin
            // Recorded only now that it is where it was named, with where that is, read
            // through its handle, and the handle: removed later only while it is still
            // that folder (RemoveMadeFolder).
            szWhere := FinalPathOfHeld(handle);
            if Kind = 0 then
            begin
               SetArrayLength(g_aFoldersMade, GetArrayLength(g_aFoldersMade) + 1);
               g_aFoldersMade[GetArrayLength(g_aFoldersMade) - 1] := missing[index];
               SetArrayLength(g_aFoldersMadeAt, GetArrayLength(g_aFoldersMade));
               g_aFoldersMadeAt[GetArrayLength(g_aFoldersMade) - 1] := szWhere;
               SetArrayLength(g_aFoldersMadeHandle, GetArrayLength(g_aFoldersMade));
               g_aFoldersMadeHandle[GetArrayLength(g_aFoldersMade) - 1] := handle;
            end
            else if Kind = 1 then
            begin
               g_szBinMade := missing[index];
               g_szBinMadeAt := szWhere;
               g_hBinMade := handle;
            end
            else
            begin
               g_szControlPanelMade := missing[index];
               g_szControlPanelMadeAt := szWhere;
               g_hControlPanelMade := handle;
            end;
            Log('Setup made ' + missing[index] + ' with permissions of its own from the start - SYSTEM and the Administrators full control, ' + szSaid +
                ', nothing inherited, the Administrators the owner - before anything is copied into it.');
            parent := handle;
            MadeHandle := handle;
         end;
      end
      else
      begin
         if status = HM_STATUS_NAME_COLLISION then
         begin
            g_bFoldersChanged := true;
            Why := missing[index] + ' was made by somebody else - a folder or a link - while setup was preparing the installation, so setup does not install into it - run setup again to have it looked at';
         end
         else if HeldIsLink(parent, bKnown) then
         begin
            g_bFoldersChanged := true;
            Why := missing[index] + ' could not be made: the folder it is made in was made a junction, a symbolic link or a mount point while setup was preparing the installation, ' +
                   'or what it is could not be read, so setup does not install there - run setup again to have it looked at';
         end
         else
            Why := missing[index] + ' could not be made (NTSTATUS ' + Format('%.8x', [status]) + ')';
         Result := false;
      end;
      index := index + 1;
   end;

   FreeWindowsMemory(pDescriptor);
   if Kind = 0 then
      g_bProgramFolderNew := Result;
end;

// Work item 78's R2: a Bin or a ControlPanel that was there and held nothing when
// CheckProgramFolders looked, before anything changed (g_bRemakeBin,
// g_bRemakeControlPanel), removed and made again by setup (MakeFolders), not hardened,
// whoever could write it: a program that opened it while another account could change
// it keeps what it opened it for whatever its permissions say after - the right to
// write its data or its attributes, with which an empty folder is made a junction, and
// setup would copy through it - and the folder made again is another, which no such
// handle reaches. Who could write it is not asked: the program folder's hardening hands
// its permissions down to it first, and what they were before is then gone. Its hold is
// let go first (LetGoOfHeld); a folder another program holds open without sharing
// delete cannot be removed, nor one something was put in since, and one held open
// sharing it is removed at once or left pending until it is closed, when making it again
// fails: each is a refusal, false with Why. Kind and MadeHandle are MakeFolders'.
function RemakeEmptyFolder(Folder: String; HeldHandle: Integer; Kind: Integer; var MadeHandle: Integer; var Why: String): Boolean;
var
   szParent: String;
   parent: Integer;
begin
   Result := false;
   Why := '';
   MadeHandle := -1;
   szParent := Copy(Folder, 1, Length(Folder) - Length(ExtractFileName(Folder)));
   if not IsVolumeRoot(szParent) then
      szParent := RemoveBackslash(szParent);
   // The folder it is in, held already by the walk and so the same folder by its path.
   parent := HoldForSetup(szParent);
   if parent = -1 then
   begin
      Why := szParent + ', the folder ' + Folder + ' is in, could not be held open (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   LetGoOfHeld(HeldHandle);
   if not RemoveDir(Folder) then
   begin
      g_bFoldersChanged := true;
      Why := Folder + ' held nothing, and setup makes it again with permissions of its own, but it could not be removed (Windows error ' + IntToStr(DLLGetLastError()) +
             '): another program may hold it open, or have put something in it since. An administrator closes that program, or restarts Windows, and runs setup again';
      Exit;
   end;
   Log('Setup removed ' + Folder + ', which was there and held nothing, to make it again with permissions of its own: a program that opened it while another account could ' +
       'change it would keep what it opened it for.');
   Result := MakeFolders(szParent, parent, Folder, Kind, MadeHandle, Why);
end;

// One folder this setup made (MakeFolders), removed when it is empty and is still the
// folder setup made: its own hold let go (Handle, LetGoOfHeld), its path opened as
// itself, a link not followed, and read through that handle - no link, and where setup
// made it (At, FinalPathOfHeld) - before it is removed by its path. Run while the
// folders above it are still held (RemoveFoldersMadeHere before ReleaseHeldFolders),
// and the folder made with permissions only the administrators may change, so that
// nobody else can put anything in its place in the meantime. What the path leads to
// otherwise - a link put on the way, another folder - is left as it is, and said.
procedure RemoveMadeFolder(Folder: String; At: String; Handle: Integer);
var
   check: Integer;
   bKnown: Boolean;
   bSame: Boolean;
begin
   LetGoOfHeld(Handle);
   check := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   if check = -1 then
   begin
      Log(Folder + ', which setup made, could not be opened again (Windows error ' + IntToStr(DLLGetLastError()) + '), and is left as it is.');
      Exit;
   end;
   bSame := (not HeldIsLink(check, bKnown)) and (At <> '') and (CompareText(FinalPathOfHeld(check), At) = 0);
   ReleaseFolder(check);
   if not bSame then
      Log(Folder + ' is not the folder setup made there now, or what it is could not be read, so setup leaves it as it is.')
   else if RemoveDir(Folder) then
      Log('Setup removed ' + Folder + ', which it had made and nothing was installed into.')
   else
      Log(Folder + ', which setup made, is not empty and stays.');
end;

// The folders this setup made (MakeFolders) that are still empty, deepest
// first, removed (RemoveMadeFolder) - after a cancel or a failure before the files,
// and when the folder page is gone back to and the preparation runs again - and
// forgotten. The holds of the walks are let go after this, not before.
procedure RemoveFoldersMadeHere();
var
   index: Integer;
begin
   // Bin and ControlPanel first, which setup made in a program folder that was there.
   if g_szBinMade <> '' then
      RemoveMadeFolder(g_szBinMade, g_szBinMadeAt, g_hBinMade);
   if g_szControlPanelMade <> '' then
      RemoveMadeFolder(g_szControlPanelMade, g_szControlPanelMadeAt, g_hControlPanelMade);
   g_szBinMade := '';
   g_szBinMadeAt := '';
   g_szControlPanelMade := '';
   g_szControlPanelMadeAt := '';

   for index := GetArrayLength(g_aFoldersMade) - 1 downto 0 do
      RemoveMadeFolder(g_aFoldersMade[index], g_aFoldersMadeAt[index], g_aFoldersMadeHandle[index]);
   SetArrayLength(g_aFoldersMade, 0);
   SetArrayLength(g_aFoldersMadeAt, 0);
   SetArrayLength(g_aFoldersMadeHandle, 0);
   g_bProgramFolderNew := false;
end;

// Whether the program folder this setup made is the one {app} names, with no link on
// the way: then section_dirs.iss gives it to the uninstaller as {app}. Made where an
// administrators' link leads, it is given by the path it was made at instead, with
// the folders above it (MadeParentCount): the uninstaller removes an empty {app}
// through its path, and where {app} is itself the link that would remove the link.
function ProgramFolderMadeAsNamed(): Boolean;
begin
   Result := false;
   if g_bProgramFolderNew then
      Result := CompareText(g_szProgramFolderReal, RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0;
end;

// For section_dirs.iss: the folders above the program folder that this setup made
// (g_aFoldersMade without its last, the program folder - with it, when the program
// folder was made where a link leads), one an entry, shallowest first, so that the
// uninstaller - which goes through what it recorded backwards - removes each, when
// empty, after the program folder and before the one above it. Eight entries: when
// setup made more, the eight nearest the program folder are given, and the setup log
// says which are not.
function MadeParentCount(): Integer;
begin
   Result := GetArrayLength(g_aFoldersMade);
   if ProgramFolderMadeAsNamed() then
      Result := Result - 1;
   if Result < 0 then
      Result := 0;
end;

function MadeParentOffset(): Integer;
begin
   Result := MadeParentCount() - 8;
   if Result < 0 then
      Result := 0;
end;

function SetupMadeFolder(Index: Integer): Boolean;
begin
   Result := g_bProgramFolderNew and (MadeParentOffset() + Index < MadeParentCount());
end;

function FolderMadeBySetup(Param: String): String;
var
   index: Integer;
begin
   index := MadeParentOffset() + StrToIntDef(Param, 0);
   Result := '';
   if (index >= 0) and (index < GetArrayLength(g_aFoldersMade)) then
      Result := g_aFoldersMade[index];
end;

function SetupMadeProgramFolder(): Boolean;
begin
   Result := ProgramFolderMadeAsNamed();
end;

// For section_dirs.iss: Bin and ControlPanel when setup made them in a program
// folder that was there (MakeFolders, kinds 1 and 2), so that the uninstaller removes
// each when it is empty.
function SetupMadeBin(): Boolean;
begin
   Result := g_szBinMade <> '';
end;

function SetupMadeControlPanel(): Boolean;
begin
   Result := g_szControlPanelMade <> '';
end;

// Whether a folder's list of permissions inherits nothing (SE_DACL_PROTECTED): what
// hMailServer.exe /ProtectFolders leaves on the data folders it makes private. False
// when it cannot be read.
function InheritsNothing(Folder: String): Boolean;
var
   pDescriptor: cardinal;
   pDacl: cardinal;
   control: Word;
   revision: cardinal;
begin
   Result := false;
   pDescriptor := 0;
   pDacl := 0;
   if ReadFolderPermissions(Folder, HM_FILE_OBJECT, HM_DACL_INFORMATION, 0, 0, pDacl, 0, pDescriptor) <> 0 then
      Exit;
   control := 0;
   revision := 0;
   if ControlOfDescriptor(pDescriptor, control, revision) then
      Result := (control and HM_DACL_PROTECTED) <> 0;
   FreeWindowsMemory(pDescriptor);
end;

// Whether the owner of a folder held open (HoldForSetup) is SYSTEM, the
// Administrators or TrustedInstaller - ServiceAccount.cpp's IsTrustedWriter_, which
// ServiceAccount::Harden asks of a program folder: any other owner may change the
// permissions again, whatever they say. Read through the handle that holds it. Known
// is false when the owner cannot be read; Who names the owner, or why it could not be
// read.
function HeldOwnerIsTrusted(HeldHandle: Integer; var Known: Boolean; var Who: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
begin
   Result := false;
   Known := false;
   Who := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Who := 'its owner could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;

   Known := pOwner <> 0;
   if Known then
   begin
      Result := IsTrustedSid(pOwner);
      Who := SidNameAt(pOwner);
   end
   else
      Who := 'it has no owner';

   FreeWindowsMemory(pDescriptor);
end;

// An entry of an existing list of permissions added to a new one as an entry of the
// new list's own: the same entry with its inherited mark taken off
// (ServiceAccount.cpp's RebuiltAcl_, copyOwn).
function AddAsOwnEntry(pNewAcl: cardinal; pAce: cardinal): Boolean;
var
   head: ACE_START;
   size: cardinal;
   pCopy: cardinal;
begin
   Result := false;
   CopyAceStart(head, pAce, 8);
   size := (head.TypeFlagsSize shr 16) and $FFFF;
   if size < 8 then
      Exit;
   pCopy := AllocateWindowsMemory(HM_LPTR, size);
   if pCopy = 0 then
      Exit;
   CopyBytes(pCopy, pAce, size);
   if (head.TypeFlagsSize and HM_INHERITED_FLAG_BIT) <> 0 then
      head.TypeFlagsSize := head.TypeFlagsSize - HM_INHERITED_FLAG_BIT;
   WriteAceStart(pCopy, head, 4);
   Result := AppendAce(pNewAcl, HM_ACL_REVISION, HM_MAXDWORD, pCopy, size);
   FreeWindowsMemory(pCopy);
end;

// The permissions of a program folder whose owner is not SYSTEM, the Administrators
// or TrustedInstaller, rebuilt as ServiceAccount.cpp's RebuiltAcl_ rebuilds them
// (ShapeHardened_, distrustOwn), and written with the Administrators as its owner:
//   - every deny entry kept, an inherited one as an entry of the folder's own;
//   - every allow entry of its own that gives no right to write
//     (HM_FOLDER_WRITE_RIGHTS), or is SYSTEM's, the Administrators' or
//     TrustedInstaller's, kept - the service's Read & execute on Bin
//     (NT SERVICE\hMailServer:(OI)(CI)(RX), from /GrantServiceAccess) among them;
//     one that lets anybody else write, or of another kind, dropped and named: that
//     owner could have written it;
//   - SYSTEM and the Administrators full control, handed down;
//   - an inherited allow entry for the service's SID that gives no right to write
//     kept as an entry of its own; every other inherited entry dropped, and named
//     when it gave a right to write;
//   - BUILTIN\Users Read & execute, handed down, when every user could read the
//     folder before (Users);
//   - for a data folder (Service: HardenDataFolder), every entry of the service's,
//     NT SERVICE\hMailServer, its own or inherited, kept as an entry of the folder's own
//     whatever it gives - the Modify /GrantServiceAccess gives it on Data, Logs, Temp and
//     Database - and no entry for Users added (Users false): /ProtectFolders makes the
//     data folders private after the files.
// Deny entries first, as Windows' canonical order has them. pDacl is the folder's
// list as it was read through the handle that holds it (HardenHeldFolder). The
// Administrators are made the owner with takeown.exe /A, which takes the folder with
// their privilege whatever its list gives them - its owner may have left them no
// right to change its owner or its permissions - and the list is then written, which
// its new owner may always do. Only the earlier installation's own folders come here
// (HardenHeldFolder, HardenDataFolder), and a folder whose permissions let another account
// change them or its owner (HardenDistrustedFolder, check 6 of R1 to R4, 28 October 2026),
// whose entries of its own that account could have written. True when it was written;
// false with Why otherwise.
function RebuildFolderOfUntrustedOwner(Folder: String; pDacl: cardinal; Users: Boolean; Service: Boolean; var Why: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pNew: cardinal;
   pAce: cardinal;
   pSid: cardinal;
   aclSize: cardinal;
   inUse: cardinal;
   count: cardinal;
   index: cardinal;
   aceType: cardinal;
   isInherited: Boolean;
   writes: Boolean;
   ok: Boolean;
   written: cardinal;
   pass: Integer;
   ResultCode: Integer;
   szDropped: String;
   szSaid: String;
begin
   Result := false;
   Why := '';
   inUse := 8;
   count := 0;
   if pDacl <> 0 then
   begin
      if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
      begin
         Why := 'its permissions could not be read';
         Exit;
      end;
      inUse := info.AclBytesInUse;
      count := info.AceCount;
   end;

   // Room for what is kept and the four entries added - SYSTEM's, the
   // Administrators', Users' and one to spare - each at most 8 bytes and a SID's 68.
   aclSize := ((inUse + 4 * 76 + 3) div 4) * 4;
   pNew := AllocateWindowsMemory(HM_LPTR, aclSize);
   if (pNew = 0) or (not StartAcl(pNew, aclSize, HM_ACL_REVISION)) then
   begin
      Why := 'its new permissions could not be made (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      if pNew <> 0 then
         FreeWindowsMemory(pNew);
      Exit;
   end;

   ok := true;
   szDropped := '';
   for pass := 0 to 2 do
   begin
      if ok and (pass = 2) then
      begin
         ok := false;
         pSid := 0;
         if SidOfText('S-1-5-18', pSid) then
         begin
            ok := AppendAllowAce(pNew, HM_ACL_REVISION, HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT, HM_FILE_ALL_ACCESS, pSid);
            FreeWindowsMemory(pSid);
         end;
         pSid := 0;
         if ok and SidOfText('S-1-5-32-544', pSid) then
         begin
            ok := AppendAllowAce(pNew, HM_ACL_REVISION, HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT, HM_FILE_ALL_ACCESS, pSid);
            FreeWindowsMemory(pSid);
         end;
      end;

      index := 0;
      while ok and (index < count) do
      begin
         pAce := 0;
         if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
         begin
            CopyAceStart(head, pAce, 8);
            aceType := head.TypeFlagsSize and $FF;
            isInherited := (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) <> 0;
            writes := (head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0;
            if pass = 0 then
            begin
               if aceType = HM_ACCESS_DENIED then
                  ok := AddAsOwnEntry(pNew, pAce);
            end
            else if pass = 1 then
            begin
               if (not isInherited) and (aceType <> HM_ACCESS_DENIED) then
               begin
                  if aceType <> HM_ACCESS_ALLOWED then
                     szDropped := szDropped + ', an entry of its own of another kind'
                  else if writes and (not IsTrustedSid(pAce + 8)) and (not (Service and SidIs(pAce + 8, HM_SERVICE_SID))) then
                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s own'
                  else
                     ok := AddAsOwnEntry(pNew, pAce);
               end;
            end
            else
            begin
               if isInherited and (aceType <> HM_ACCESS_DENIED) then
               begin
                  if aceType <> HM_ACCESS_ALLOWED then
                     szDropped := szDropped + ', an inherited entry of another kind'
                  else if SidIs(pAce + 8, HM_SERVICE_SID) and (Service or (not writes)) then
                     ok := AddAsOwnEntry(pNew, pAce)
                  else if writes and (not SidIs(pAce + 8, 'S-1-5-18')) and (not SidIs(pAce + 8, 'S-1-5-32-544')) then
                     szDropped := szDropped + ', ' + SidNameAt(pAce + 8) + '''s inherited';
               end;
            end;
         end
         else
            ok := false;
         index := index + 1;
      end;
   end;

   szSaid := 'no entry for Users, as no user could read it';
   if Service then
      szSaid := 'no entry for Users, as for a data folder, and the service''s entries kept';
   if ok and Users then
   begin
      pSid := 0;
      ok := false;
      if SidOfText('S-1-5-32-545', pSid) then
      begin
         ok := AppendAllowAce(pNew, HM_ACL_REVISION, HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT, HM_RIGHTS_READ_EXECUTE, pSid);
         FreeWindowsMemory(pSid);
      end;
      szSaid := 'Users Read & execute';
   end;

   if not ok then
   begin
      Why := 'its new permissions could not be made';
      FreeWindowsMemory(pNew);
      Exit;
   end;

   // The Administrators the owner, with their privilege; then the list, which the
   // owner may always write.
   ResultCode := -1;
   if not Exec(ExpandConstant('{sys}\takeown.exe'), '/F "' + Folder + '" /A', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      Why := 'takeown.exe could not be run to give it to the Administrators: ' + SysErrorMessage(ResultCode)
   else if ResultCode <> 0 then
      Why := 'takeown.exe could not give it to the Administrators (it returned ' + IntToStr(ResultCode) + ')'
   else
   begin
      written := WriteFolderSecurity(Folder, HM_FILE_OBJECT, HM_PROTECTED_DACL_INFORMATION, 0, 0, pNew, 0);
      Result := written = 0;
      if szDropped = '' then
         szDropped := ', none';
      if Result then
         Log(Folder + ' was owned by an account other than SYSTEM, the Administrators or TrustedInstaller, or its permissions let one change them or its owner, so that the ' +
             'entries of its own may be that account''s: it is now the Administrators'', with SYSTEM and the Administrators full control, ' + szSaid + ', nothing inherited, and ' +
             'the entries of its own that give no right to write kept, the service''s among them. Entries that gave a right to write and were dropped: ' +
             Copy(szDropped, 3, Length(szDropped)) + '.')
      else
         Why := 'it was given to the Administrators, but its new permissions could not be written (Windows error ' + IntToStr(written) + ')';
   end;

   FreeWindowsMemory(pNew);
end;

// Whether an allow entry gives a right to write (HM_FOLDER_WRITE_RIGHTS) to a group of
// every user (BroadGroupSid): an entry the program folder, Bin and ControlPanel may not
// keep - work item 78's very shape, an entry handed down from a drive's root and kept
// as the folder's own when its inheritance was turned off with what it had
// (icacls /inheritance:d, Explorer's "convert").
function LetsEveryUserWrite(pAce: cardinal): Boolean;
var
   head: ACE_START;
begin
   CopyAceStart(head, pAce, 8);
   Result := false;
   if (head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED then
      if (head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0 then
         Result := IsBroadSid(pAce + 8);
end;

// Work item 78's R4: the entries of a folder's own, after setup hardened it, that let
// an account other than SYSTEM, the Administrators or TrustedInstaller change what is
// in it, or what is made in it - entries an administrator gave a named account or
// group, which setup keeps on a folder the administrators own, as
// ServiceAccount::Harden keeps them, and which hMailServer.exe /ProtectFolders' CHECK
// names and the live update's check of the installation (AdminUpdateChecks) refuses;
// a group of every user's are taken away before this (HardenHeldFolder) - each said
// in the setup log, with how to take it away, and added to g_szFolderFindings for the
// box an attended installation shows. Read through the handle that holds the folder.
// CREATOR OWNER is not said: what it hands down goes to whoever makes a file there,
// and only the administrators may. OWNER RIGHTS is the owner's, one of the three.
procedure SayWriteEntriesKept(Folder: String; HeldHandle: Integer);
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pOwner: cardinal;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pAce: cardinal;
   index: cardinal;
   szSid: String;
   szText: String;
begin
   pOwner := 0;
   pDescriptor := 0;
   pDacl := 0;
   if ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then
      Exit;
   if (pDacl <> 0) and ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      index := 0;
      while index < info.AceCount do
      begin
         pAce := 0;
         if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
         begin
            CopyAceStart(head, pAce, 8);
            if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and
               (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) then
            begin
               szSid := TextOfSidAt(pAce + 8);
               szText := Folder + ' keeps an entry of its own that lets ' + SidNameAt(pAce + 8) + ' change what is in it: setup keeps the entries an administrator gave a named ' +
                         'account or group on a folder the administrators own, as hMailServer.exe /ProtectFolders does, so that account may change the programs there. ' +
                         'Unless that is meant, an administrator takes it away: icacls "' + Folder + '" /remove:g *' + szSid + '.';
               Log('The program folder: ' + szText);
               g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;
            end;
         end;
         index := index + 1;
      end;
   end;
   FreeWindowsMemory(pDescriptor);
end;

// Whether a folder setup hardened is as it means to leave it, read again through the
// handle that holds it: its list inherits nothing, its owner is SYSTEM, the
// Administrators or TrustedInstaller, and no entry lets a group of every user write
// (LetsEveryUserWrite) - nor, when AnyAccount is set (a folder whose list setup
// rebuilt), any account other than the three, and the service when ServiceMay is set
// (a data folder, HardenDataFolder). Why says what is not.
function HeldFolderIsHardened(HeldHandle: Integer; AnyAccount: Boolean; ServiceMay: Boolean; var Why: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pOwner: cardinal;
   pDescriptor: cardinal;
   pDacl: cardinal;
   pAce: cardinal;
   index: cardinal;
   readResult: cardinal;
   control: Word;
   revision: cardinal;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDescriptor := 0;
   pDacl := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'its permissions could not be read again (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   control := 0;
   revision := 0;
   if not ControlOfDescriptor(pDescriptor, control, revision) then
      Why := 'its permissions could not be read again'
   else if (control and HM_DACL_PROTECTED) = 0 then
      Why := 'it still inherits from the folder above'
   else if not IsTrustedSid(pOwner) then
      Why := 'it is still owned by ' + SidNameAt(pOwner)
   else if pDacl = 0 then
      Why := 'it has no list of permissions at all'
   else if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
      Why := 'its permissions could not be read again'
   else
   begin
      Result := true;
      index := 0;
      while Result and (index < info.AceCount) do
      begin
         pAce := 0;
         if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
         begin
            CopyAceStart(head, pAce, 8);
            if LetsEveryUserWrite(pAce) then
            begin
               Result := false;
               Why := SidNameAt(pAce + 8) + ' may still change what is in it';
            end
            else if AnyAccount and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and
                    (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then
            begin
               Result := false;
               Why := SidNameAt(pAce + 8) + ' may still change what is in it';
            end;
         end
         else
         begin
            Result := false;
            Why := 'an entry of its permissions could not be read again';
         end;
         index := index + 1;
      end;
   end;
   FreeWindowsMemory(pDescriptor);
end;

// One folder of the program, there before this installation, hardened in place while
// it is held (PrepareProgramFolders), as ServiceAccount::Harden hardens it after the
// files: SYSTEM and the Administrators full control and BUILTIN\Users Read & execute
// where every user could read it before (EveryUserMayRead), handed down, and nothing
// inherited. Its owner and list are read through the handle that holds it (HeldHandle),
// which names that folder whatever is done to its path. Owned by SYSTEM, the
// Administrators or TrustedInstaller, it keeps the entries of its own, as Harden keeps
// them - icacls on the SIDs - but for those that let a group of every user write,
// which are taken away (icacls /remove:g on each such group, before the rest); what of
// the rest lets another account write is said (SayWriteEntriesKept). Owned by anybody
// else, it is taken only when it is the earlier installation's (Adopt), whose folders
// an administrator's own account may own: given to the Administrators and its
// permissions rebuilt (RebuildFolderOfUntrustedOwner), the entries that give no right
// to write kept, as Harden keeps them. Any other is not taken - its owner may hold it
// open with the right to change its permissions, which a handle keeps whoever owns the
// folder after - and CheckProgramFolders has refused it already. The folder is then
// read again (HeldFolderIsHardened). True when it was hardened; false with Why, and
// setup does not install, otherwise.
function HardenHeldFolder(Folder: String; HeldHandle: Integer; Adopt: Boolean; var Why: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   ResultCode: Integer;
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   pAce: cardinal;
   readResult: cardinal;
   index: cardinal;
   bUsers: Boolean;
   bTrusted: Boolean;
   szUsers: String;
   szSaid: String;
   szRemove: String;
   szSid: String;
   szOwner: String;
   szText: String;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'its owner and permissions could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if pOwner = 0 then
   begin
      Why := 'its owner could not be read';
      FreeWindowsMemory(pDescriptor);
      Exit;
   end;

   bUsers := EveryUserMayRead(pDacl);
   szUsers := '';
   szSaid := 'no entry for Users, as no user could read it';
   if bUsers then
   begin
      szUsers := ' *S-1-5-32-545:(OI)(CI)RX';
      szSaid := 'Users Read & execute';
   end;
   bTrusted := IsTrustedSid(pOwner);
   szOwner := SidNameAt(pOwner);

   if bTrusted then
   begin
      // The groups of every user that an entry of its own lets write, each once.
      szRemove := '';
      if (pDacl <> 0) and ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
      begin
         index := 0;
         while index < info.AceCount do
         begin
            pAce := 0;
            if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
            begin
               CopyAceStart(head, pAce, 8);
               if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0) and LetsEveryUserWrite(pAce) then
               begin
                  szSid := TextOfSidAt(pAce + 8);
                  if (szSid <> '') and (Pos(' *' + szSid + ' ', szRemove + ' ') = 0) then
                     szRemove := szRemove + ' *' + szSid;
               end;
            end;
            index := index + 1;
         end;
      end;
      FreeWindowsMemory(pDescriptor);

      while szRemove <> '' do
      begin
         szSid := Copy(szRemove, 2, Length(szRemove));
         if Pos(' ', szSid) > 0 then
         begin
            szRemove := Copy(szSid, Pos(' ', szSid), Length(szSid));
            szSid := Copy(szSid, 1, Pos(' ', szSid) - 1);
         end
         else
            szRemove := '';
         if not Exec(ExpandConstant('{sys}\icacls.exe'), '"' + Folder + '" /remove:g ' + szSid, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
         begin
            Why := 'icacls could not be run to take ' + szSid + '''s entry away: ' + SysErrorMessage(ResultCode);
            Exit;
         end;
         if ResultCode <> 0 then
         begin
            Why := 'icacls could not take ' + szSid + '''s entry away (it returned ' + IntToStr(ResultCode) + ')';
            Exit;
         end;
         Log('icacls took away the entries of ' + Folder + '''s own for ' + szSid + ', a group of every user of this computer, which let it change what is in it: ' +
             'every user could otherwise change the programs there (work item 78).');
      end;

      if not Exec(ExpandConstant('{sys}\icacls.exe'), '"' + Folder + '" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F' + szUsers,
                  '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      begin
         Why := 'icacls could not be run: ' + SysErrorMessage(ResultCode);
         Exit;
      end;
      Log('icacls hardened ' + Folder + ' before the files - SYSTEM and the Administrators full control, ' + szSaid + ', nothing inherited, the entries of its own kept - and returned ' + IntToStr(ResultCode) + '.');
      if ResultCode <> 0 then
      begin
         Why := 'icacls could not change its permissions (it returned ' + IntToStr(ResultCode) + ')';
         Exit;
      end;
   end
   else if not Adopt then
   begin
      Why := 'it was made by ' + SidNameAt(pOwner) + ', who may still hold it open with the right to change its permissions, and so it is not taken';
      FreeWindowsMemory(pDescriptor);
      Exit;
   end
   else
   begin
      Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, bUsers, false, Why);
      FreeWindowsMemory(pDescriptor);
      if not Result then
         Exit;
      Result := false;
   end;

   if not HeldFolderIsHardened(HeldHandle, not bTrusted, false, Why) then
   begin
      Why := 'once setup had changed its permissions, ' + Why;
      Exit;
   end;
   if bTrusted then
      SayWriteEntriesKept(Folder, HeldHandle)
   else
   begin
      // A handle its owner opened before keeps the right to change its permissions,
      // which only closing it, or a restart, takes away: setup does not install into it
      // until Windows has restarted (HardenForRestart, the only caller that adopts one).
      szText := Folder + ' was owned by ' + szOwner + ', and setup gave it to the Administrators and rebuilt its permissions, but a program that account started before may ' +
                'still hold it open with the right to change its permissions, which only a restart takes away: setup installs there once Windows has restarted.';
      Log('The program folder: ' + szText);
      g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;
   end;
   Result := true;
end;

// Whether an entry of a list's own - not inherited - lets an account other than
// SYSTEM, the Administrators or TrustedInstaller do any of Rights, to what it is on or
// to what is made in it: CREATOR OWNER's is the maker's, and only the administrators
// make anything in Bin and ControlPanel once setup has hardened them; OWNER RIGHTS is
// the owner's, held to the three before this is asked; and, when ServiceMay is set,
// NT SERVICE\hMailServer's (HM_SERVICE_SID), which /GrantServiceAccess gives Modify
// on the REST API's key store, the one file of the program folder the service writes.
// Who is that account.
function OwnEntriesLetOthers(pDacl: cardinal; Rights: cardinal; ServiceMay: Boolean; var Who: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
   aceType: cardinal;
begin
   Result := false;
   Who := '';
   if pDacl = 0 then
   begin
      Result := true;
      Who := 'everyone (it has no list of permissions at all)';
      Exit;
   end;
   if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      Result := true;
      Who := 'an account in a list of permissions that could not be read';
      Exit;
   end;
   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         aceType := head.TypeFlagsSize and $FF;
         if (((head.TypeFlagsSize shr 8) and $FF) and HM_INHERITED_ACE) = 0 then
         begin
            if aceType = HM_ACCESS_ALLOWED then
            begin
               if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-0')) and (not SidIs(pAce + 8, 'S-1-3-4')) and
                  (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) then
               begin
                  Result := true;
                  Who := SidNameAt(pAce + 8);
               end;
            end
            else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then
            begin
               Result := true;
               Who := 'an account in an entry of a form setup does not read';
            end;
         end;
      end
      else
      begin
         Result := true;
         Who := 'an account in an entry that could not be read';
      end;
      index := index + 1;
   end;
end;

// Whether the folder whose owner and list were read as pFolderDescriptor and pFolderDacl
// hands a right to write (HM_FOLDER_WRITE_RIGHTS) down to the SID at pSid by an allow
// entry with the inheritance Inherit names - HM_OBJECT_INHERIT, what the files made in it
// inherit and the folders in it inherit to hand down to theirs, or HM_CONTAINER_INHERIT,
// what applies to the folders made in it (check 7 of R1 to R4, 28 October 2026). A folder
// with no list at all hands everything down; one whose list could not be read
// (pFolderDescriptor 0) is taken to hand nothing down.
function HandsDownWriteTo(pFolderDescriptor: cardinal; pFolderDacl: cardinal; pSid: cardinal; Inherit: cardinal): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
begin
   Result := false;
   if pFolderDescriptor = 0 then
      Exit;
   if pFolderDacl = 0 then
   begin
      Result := true;
      Exit;
   end;
   if not ReadAclSize(pFolderDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
      Exit;
   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pFolderDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         if ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and ((((head.TypeFlagsSize shr 8) and $FF) and Inherit) <> 0) and
            ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and SidsAreEqual(pAce + 8, pSid) then
            Result := true;
      end;
      index := index + 1;
   end;
end;

// Work item 78's R4 and the restart rule (check 5 of R1 to R4, 27 October 2026): whether an
// inherited entry of a file's list (pDacl) that applies to it lets an account other than
// SYSTEM, the Administrators or TrustedInstaller write it (HM_FOLDER_WRITE_RIGHTS) - the
// service's on the key store aside (ServiceMay), OWNER RIGHTS the owner's, held to the three
// before this is asked - when the folder it is in (pFolderDescriptor, pFolderDacl) hands that
// account no such right down (HandsDownWriteTo): an entry the file kept from where it was
// made, since a file moved into a folder keeps the entries it inherited there, still marked
// inherited. Hardening the folder rewrites such an entry from the folder's, but a handle
// opened while it applied keeps the right. A folder (Folder set) moved so keeps its entries
// as a file does, and so does every file in it, which the folder then hands down (check 7 of
// R1 to R4, 28 October 2026): its inherited entries are asked too, one that applies to it
// against what the folder it is in hands down to folders (HM_CONTAINER_INHERIT), and one
// that only hands down to what is in it (inherit-only) against what that folder hands down
// at all - to files (HM_OBJECT_INHERIT), which is how such an entry comes to a folder, or
// to folders, as CREATOR OWNER's does. Who is that account.
function StaleEntryLetsOthers(pDacl: cardinal; pFolderDescriptor: cardinal; pFolderDacl: cardinal; ServiceMay: Boolean; Folder: Boolean; var Who: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
   aceFlags: cardinal;
   inherit: cardinal;
begin
   Result := false;
   Who := '';
   if pDacl = 0 then
      Exit;
   if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
      Exit;
   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         aceFlags := (head.TypeFlagsSize shr 8) and $FF;
         inherit := HM_OBJECT_INHERIT;
         if Folder then
         begin
            if (aceFlags and HM_INHERIT_ONLY) = 0 then
               inherit := HM_CONTAINER_INHERIT
            else
               inherit := HM_OBJECT_INHERIT or HM_CONTAINER_INHERIT;
         end;
         if ((aceFlags and HM_INHERITED_ACE) <> 0) and (Folder or ((aceFlags and HM_INHERIT_ONLY) = 0)) and ((head.TypeFlagsSize and $FF) = HM_ACCESS_ALLOWED) and
            ((head.Mask and HM_FOLDER_WRITE_RIGHTS) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and
            (not (ServiceMay and SidIs(pAce + 8, HM_SERVICE_SID))) and (not HandsDownWriteTo(pFolderDescriptor, pFolderDacl, pAce + 8, inherit)) then
         begin
            Result := true;
            Who := SidNameAt(pAce + 8);
         end;
      end;
      index := index + 1;
   end;
end;

// A program, a library, a script or a settings file there before this installation whose
// inherited entry another account may write it by (StaleEntryLetsOthers), found in the
// first look (CheckWhatIsInside, CheckWhatElseIsInside), for NoteFolderOthersCouldChange,
// which notes the folder it is in for HardenForRestart.
procedure NoteStaleItem(Item: String; Who: String);
begin
   SetArrayLength(g_aStaleItems, GetArrayLength(g_aStaleItems) + 1);
   g_aStaleItems[GetArrayLength(g_aStaleItems) - 1] := Item;
   SetArrayLength(g_aStaleWhat, GetArrayLength(g_aStaleItems));
   g_aStaleWhat[GetArrayLength(g_aStaleItems) - 1] := Item + ' let ' + Who + ' change it, by an entry it kept from where it was made, which the folder it is in does not ' +
                                                       'hand down';
   Log('The program folder: ' + Item + ' has an inherited entry that lets ' + Who + ' change it, which the folder it is in does not hand down: one it kept from where it ' +
       'was made, so that a program that account started may hold it open with that right.');
end;

// One folder for HardenForRestart (NoteFolderOthersCouldChange, CheckWhatElseIsInside,
// DataLinkLeadsWhereAllowed), once: the folder; the handle that holds it, or -1 for one
// HardenForRestart holds itself; whether it is a data folder, taken keeping the service's
// entries (HardenDataFolder); whether another account could change its permissions or its
// owner, so that no entry of its own that lets another account write is kept
// (Distrust, HardenDistrustedFolder); and what another account could do, said in the
// setup log and in the refusal.
procedure AddRestartFolder(Folder: String; Handle: Integer; Data: Boolean; Distrust: Boolean; What: String);
var
   index: Integer;
   count: Integer;
begin
   for index := 0 to GetArrayLength(g_aRestartFolders) - 1 do
      if CompareText(g_aRestartFolders[index], Folder) = 0 then
         Exit;
   count := GetArrayLength(g_aRestartFolders) + 1;
   SetArrayLength(g_aRestartFolders, count);
   SetArrayLength(g_aRestartHandles, count);
   SetArrayLength(g_aRestartData, count);
   SetArrayLength(g_aRestartDistrust, count);
   g_aRestartFolders[count - 1] := Folder;
   g_aRestartHandles[count - 1] := Handle;
   g_aRestartData[count - 1] := Data;
   g_aRestartDistrust[count - 1] := Distrust;
   if g_szRestartWhy <> '' then
      g_szRestartWhy := g_szRestartWhy + '; ';
   g_szRestartWhy := g_szRestartWhy + What;
   Log('The program folder: ' + What + ' before this installation.');
end;

// Work item 78's R2 and R4 (check 5 of R1 to R4, 27 October 2026): whether a data folder -
// Data, Logs, Temp or Database of the program folder (IsDataFolderName), or the folder such
// a folder's link leads to (DataLinkLeadsWhereAllowed) - held open as itself (Handle) is
// SYSTEM's, the Administrators', TrustedInstaller's or the service's, NT SERVICE\hMailServer,
// which makes what is in it, and has no entry of its own that lets an account other than
// those write it (OwnEntriesLetOthers; ServiceMay, since /GrantServiceAccess gives the
// service Modify there). hMailServer.exe /ProtectFolders makes a data folder private after
// the files but keeps every entry of its own and never changes its owner, so that such an
// account would keep the message store, the logs, the messages being scanned or the
// built-in database's folder after the installation. Read through the handle. Owner says it
// is its owner that is not - which, for the earlier installation's, setup takes before a
// restart (HardenForRestart, HardenDataFolder) - Who what is not, said of the folder
// ("is owned by ..."), and Remedy what an administrator does. True when it is as it may be.
function DataFolderIsAllowed(Folder: String; Handle: Integer; var Owner: Boolean; var Who: String; var Remedy: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   szText: String;
   szSid: String;
begin
   Result := false;
   Owner := false;
   Who := '';
   Remedy := 'An administrator takes it - takeown /F "' + Folder + '" /A - checks it, restarts Windows and runs setup again.';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(Handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Who := 'is there, and its owner and permissions could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if pOwner = 0 then
      Who := 'is there, and its owner could not be read'
   else if (not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID)) then
   begin
      Owner := true;
      Who := 'is owned by ' + SidNameAt(pOwner) + ', an account other than SYSTEM, the Administrators, TrustedInstaller or the service, which may change it, and what the ' +
             'service keeps there, whatever its permissions say';
      Remedy := 'An administrator checks that it is meant to be there and gives it to the Administrators - icacls "' + Folder + '" /setowner *S-1-5-32-544 - or removes it, ' +
                'restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
   end
   else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then
   begin
      Who := 'has an entry of its own that lets ' + szText + ' change it and what the service keeps there';
      szSid := SidInName(szText);
      if szSid <> '' then
         szSid := 'icacls "' + Folder + '" /remove:g *' + szSid
      else
         szSid := 'icacls "' + Folder + '" /remove:g with that account''s SID';
      Remedy := 'An administrator checks it and takes that entry away - ' + szSid + ' - restarts Windows, since a program that account started may still hold it open with ' +
                'the right to change it, and runs setup again.';
   end
   else
      Result := true;
   FreeWindowsMemory(pDescriptor);
end;

// Work item 78's R4 (check 5 of R1 to R4, 27 October 2026): the files of the program folder's
// Database - the built-in database, whose settings name, among the rest, the program the
// service runs to scan for viruses - held to the rule a Database link's target holds them
// to (DatabaseTargetIsTheAdministrators) by their owners and their own entries: each
// SYSTEM's, the Administrators', TrustedInstaller's or the service's, with no entry of its
// own that lets another account but the service write it, and no link among them. What a
// file inherits there is the program folder's, which setup hardens, and holds to the
// restart rule before (OthersCouldChange). '' when so; otherwise what is not, said of the
// Database ("holds ..."), with Remedy.
function DatabaseFilesWhy(Folder: String; var Remedy: String): String;
var
   rec: TFindRec;
   handle: Integer;
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   szItem: String;
   szText: String;
begin
   Result := '';
   Remedy := '';
   if not FindFirst(AddBackslash(Folder) + '*', rec) then
   begin
      Result := 'is there, and what is in it could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Remedy := 'An administrator takes it - takeown /F "' + Folder + '" /A /R /D Y - checks it, restarts Windows and runs setup again.';
      Exit;
   end;
   try
      repeat
         if (Result = '') and ((rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then
         begin
            szItem := AddBackslash(Folder) + rec.Name;
            Remedy := 'An administrator takes it - takeown /F "' + szItem + '" /A - checks it, restarts Windows and runs setup again.';
            if (rec.Attributes and HM_REPARSE_POINT) <> 0 then
            begin
               Result := 'holds ' + szItem + ', which is a link';
               Remedy := 'An administrator checks it and removes the link - del "' + szItem + '" - restarts Windows, and runs setup again.';
            end
            else
            begin
               handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
               if handle = -1 then
                  Result := 'holds ' + szItem + ', whose owner could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')'
               else
               begin
                  pOwner := 0;
                  pDacl := 0;
                  pDescriptor := 0;
                  if ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then
                     Result := 'holds ' + szItem + ', whose owner could not be read'
                  else
                  begin
                     if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then
                     begin
                        Result := 'holds ' + szItem + ', which is owned by ' + SidNameAt(pOwner) + ', an account other than SYSTEM, the Administrators, TrustedInstaller or ' +
                                  'the service, which may change the built-in database whose settings name the program the service runs to scan for viruses';
                        Remedy := 'An administrator checks that it is meant to be there and gives it to the Administrators - icacls "' + szItem + '" /setowner *S-1-5-32-544 - ' +
                                  'restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
                     end
                     else if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, true, szText) then
                     begin
                        Result := 'holds ' + szItem + ', which has an entry of its own that lets ' + szText + ' change the built-in database whose settings name the ' +
                                  'program the service runs to scan for viruses';
                        Remedy := 'An administrator checks it and gives it the permissions of the folder it is in - icacls "' + szItem + '" /reset - restarts Windows, ' +
                                  'since a program that account started may still hold it open with the right to change it, and runs setup again.';
                     end;
                     FreeWindowsMemory(pDescriptor);
                  end;
                  ReleaseFolder(handle);
               end;
            end;
         end;
      until (Result <> '') or (not FindNext(rec));
   finally
      FindClose(rec);
   end;
   if Result = '' then
      Remedy := '';
end;

// Work item 78's R2 (check 5 of R1 to R4, 27 October 2026), for HardenForRestart: a data
// folder of the earlier installation, or the folder such a folder's link leads to, that
// another account owned (DataFolderIsAllowed) - an administrator's own account may own the
// earlier installation's data folders, under Windows' policy that makes the maker the owner
// - given to the Administrators and its permissions rebuilt with the service's entries kept
// and no entry for Users (RebuildFolderOfUntrustedOwner, Service), then read again through
// the handle that holds it (HeldFolderIsHardened). The installation is then refused until
// Windows has restarted, as for Bin. True when it was done; false with Why otherwise.
function HardenDataFolder(Folder: String; HeldHandle: Integer; var Why: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   szOwner: String;
   szText: String;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'its owner and permissions could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   szOwner := SidNameAt(pOwner);
   Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, false, true, Why);
   FreeWindowsMemory(pDescriptor);
   if not Result then
      Exit;
   Result := HeldFolderIsHardened(HeldHandle, true, true, Why);
   if not Result then
   begin
      Why := 'once setup had changed its permissions, ' + Why;
      Exit;
   end;
   szText := Folder + ' was owned by ' + szOwner + ', and setup gave it to the Administrators and rebuilt its permissions, the service''s entries kept, but a program that ' +
             'account started before may still hold it open with the right to change it, which only a restart takes away: setup installs there once Windows has restarted.';
   Log('The program folder: ' + szText);
   g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;
end;

// Whether a file's name is that of a program or a library: what the service, the
// administrators' tools and the Control Panel load and run from Bin and ControlPanel
// (AdminUpdateChecks' CheckFolderTree_ holds the same two to its rule).
function IsProgramFile(Name: String): Boolean;
var
   szExtension: String;
begin
   szExtension := Lowercase(ExtractFileExt(Name));
   Result := (szExtension = '.exe') or (szExtension = '.dll');
end;

// Whether a file's name is that of a script something of the installation runs: the
// event script the service runs from Events (EventHandlers.vbs, or a JScript one), the
// SQL DBSetup runs from DBScripts, the PHP of WebAdmin, and a script of PowerShell or of
// the command prompt an administrator may keep beside them.
function IsScriptFile(Name: String): Boolean;
var
   szExtension: String;
begin
   szExtension := Lowercase(ExtractFileExt(Name));
   Result := (szExtension = '.vbs') or (szExtension = '.js') or (szExtension = '.sql') or (szExtension = '.php') or (szExtension = '.ps1') or
             (szExtension = '.cmd') or (szExtension = '.bat');
end;

// Whether a file's name is that of one that tells the service, the administrators'
// tools or the uninstaller what to run, or where: Bin's hMailServer.INI, whose
// [Directories] name the folder of the event scripts the service runs and its data,
// and the REST API's key store, hMailServerApiKeys.ini; a .config or a .json beside a
// program, which .NET reads before it loads the program's libraries and may name
// where to look for them; and the uninstaller's record of the installation,
// unins000.dat, which the uninstaller reads elevated. Held to the rule of a program
// (CheckWhatIsInside, CheckWhatElseIsInside), as the live update holds
// hMailServer.INI to it (AdminUpdateChecks).
function IsSettingsFile(Name: String): Boolean;
var
   szExtension: String;
begin
   szExtension := Lowercase(ExtractFileExt(Name));
   Result := (CompareText(Name, 'hMailServer.INI') = 0) or (CompareText(Name, 'hMailServerApiKeys.ini') = 0) or (szExtension = '.config') or (szExtension = '.json') or
             ((CompareText(Copy(Name, 1, 5), 'unins') = 0) and (szExtension = '.dat'));
end;

// Whether a folder of the program folder's own level is one setup installs into that
// holds what the service or the administrators run - the tools and the importer
// (Addons), the event script (Events), the SQL DBSetup runs (DBScripts), the PHP of
// WebAdmin and the languages the tools read (Languages) - which may not be a link
// (CheckWhatElseIsInside): setup would install into, and the service and the
// administrators run from, a folder it does not prepare.
function IsRunFolderName(Name: String): Boolean;
begin
   Result := (CompareText(Name, 'Addons') = 0) or (CompareText(Name, 'Events') = 0) or (CompareText(Name, 'DBScripts') = 0) or (CompareText(Name, 'WebAdmin') = 0) or
             (CompareText(Name, 'Languages') = 0);
end;

// Whether a folder in the program folder is one of the data folders the service writes
// in - Data, Logs, Temp, Database - which /GrantServiceAccess gives the service Modify
// on, and the service may own what it made there; setup does not look inside them.
function IsDataFolderName(Name: String): Boolean;
begin
   Result := (CompareText(Name, 'Data') = 0) or (CompareText(Name, 'Logs') = 0) or (CompareText(Name, 'Temp') = 0) or (CompareText(Name, 'Database') = 0);
end;

// Work item 78's R4: what is in Bin or ControlPanel, there before this installation,
// at any depth - the scope of the live update's check of an installation
// (AdminUpdateChecks' CheckFolderTree_) - and in the program folder's other folders that
// hold what the service and the administrators run (CheckWhatElseIsInside, Scripts set:
// a script, IsScriptFile, counts as a program there), each read through a handle opened
// on it as itself. Refused, each named with what to do, in Why:
//   - a link (a junction, a symbolic link, a mount point, any reparse point): what is
//     reached through it is not in the folder, and setup would install, and the
//     service and the administrators run, through it - the live update refuses one too;
//   - a program or a library (IsProgramFile), a file that says what they run or where
//     (IsSettingsFile: the settings file, the service's key store, a .config or a .json
//     beside a program), or a folder, whose owner is not SYSTEM, the Administrators or
//     TrustedInstaller, or cannot be read - its owner may change it whatever its
//     permissions say, and setup replaces only the files it installs - or one an entry
//     of whose own lets another account change it (OwnEntriesLetOthers); the key store
//     may also be the service's, and have its entry, which /GrantServiceAccess gives it.
// In the first look, a program, a library, a script, a settings file or a folder whose
// inherited entry, kept from where it was made, lets another account change it, which the
// folder it is in does not hand down (StaleEntryLetsOthers), is noted for the restart rule
// (NoteStaleItem) - a folder moved in keeps its entries, and so does every file in it
// (check 7 of R1 to R4, 28 October 2026).
// Another file - a note - whose owner is another account, or cannot be read, is said
// in the setup log and added to g_szFolderFindings when Final is set; the service's own
// files are not. Found counts
// what was refused. Run before the service is stopped, and again once Bin and
// ControlPanel are hardened (Final), when only the administrators can add to them.
// Each folder is opened as itself and held while it is looked at, a link not followed,
// and refused unless it is no link and - below the first, Expected - the folder of its
// name in the one above, read through the two handles (FinalPathOfHeld): what is walked,
// and an empty folder removed in the last look (RemoveHeldFolder), is only ever what was
// listed, never what a link put on the way since leads to (check 4 of R1 to R4, 27
// October 2026).
function CheckWhatIsInside(Folder: String; Expected: String; Depth: Integer; Final: Boolean; Scripts: Boolean; var Found: Integer; var Why: String): Boolean;
var
   rec: TFindRec;
   folders: array of String;
   count: Integer;
   index: Integer;
   handle: Integer;
   folderHandle: Integer;
   bKnown: Boolean;
   szFinal: String;
   szRemoved: String;
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   bFolder: Boolean;
   bCode: Boolean;
   bLink: Boolean;
   bKeyStore: Boolean;
   szItem: String;
   szWho: String;
   szText: String;
   szRemedy: String;
   pFolderOwner: cardinal;
   pFolderDacl: cardinal;
   pFolderDescriptor: cardinal;
begin
   Result := true;
   count := 0;
   if Depth > 32 then
   begin
      Found := Found + 1;
      Why := Why + Folder + ' is more than 32 folders deep in the program folder, and setup does not look further. ';
      Result := false;
      Exit;
   end;
   folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   szFinal := '';
   if folderHandle <> -1 then
      if not HeldIsLink(folderHandle, bKnown) then
         szFinal := FinalPathOfHeld(folderHandle);
   if (szFinal = '') or ((Expected <> '') and (CompareText(szFinal, Expected) <> 0)) then
   begin
      if folderHandle <> -1 then
         ReleaseFolder(folderHandle);
      Found := Found + 1;
      szText := Folder + ' is not the folder setup listed - it could not be opened as itself, it is a junction, a symbolic link or a mount point now, or its path leads elsewhere - ' +
                'so setup does not look through it. An administrator looks at it, restarts Windows and runs setup again.';
      Why := Why + szText + ' ';
      Log('The program folder: refused, ' + szText);
      Result := false;
      Exit;
   end;
   if not FindFirst(AddBackslash(Folder) + '*', rec) then
   begin
      ReleaseFolder(folderHandle);
      Found := Found + 1;
      Why := Why + 'what is in ' + Folder + ' could not be read (Windows error ' + IntToStr(DLLGetLastError()) + '); an administrator takes it - takeown /F "' + Folder + '" /A /R /D Y - and runs setup again. ';
      Result := false;
      Exit;
   end;
   // What the folder hands down to the files and the folders in it, for an entry one of them
   // kept from where it was made (StaleEntryLetsOthers); pFolderDescriptor 0 when it cannot
   // be read.
   pFolderOwner := 0;
   pFolderDacl := 0;
   pFolderDescriptor := 0;
   if ReadHeldSecurity(folderHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pFolderOwner, 0, pFolderDacl, 0, pFolderDescriptor) <> 0 then
      pFolderDescriptor := 0;
   try
      repeat
         if (rec.Name <> '.') and (rec.Name <> '..') then
         begin
            szItem := AddBackslash(Folder) + rec.Name;
            bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
            bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;
            bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsSettingsFile(rec.Name) or (Scripts and IsScriptFile(rec.Name)));
            bKeyStore := (not bFolder) and (CompareText(rec.Name, 'hMailServerApiKeys.ini') = 0);
            szWho := '';
            szRemedy := '';
            if bLink then
            begin
               szWho := 'is a link - a junction, a symbolic link, a mount point or another reparse point - so what is reached through it is not in the program folder';
               if bFolder then
                  szRemedy := 'An administrator checks it and removes the link - rmdir "' + szItem + '", which leaves what it leads to as it is - restarts Windows, and runs setup again, which installs what belongs there in its place.'
               else
                  szRemedy := 'An administrator checks it and removes the link - del "' + szItem + '" - restarts Windows, and runs setup again, which installs what belongs there in its place.';
            end
            else
            begin
               handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
               if handle = -1 then
                  szWho := 'its owner could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')'
               else
               begin
                  pOwner := 0;
                  pDacl := 0;
                  pDescriptor := 0;
                  readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
                  if readResult <> 0 then
                     szWho := 'its owner could not be read (Windows error ' + IntToStr(readResult) + ')'
                  else
                  begin
                     if pOwner = 0 then
                        szWho := 'its owner could not be read'
                     else if (not IsTrustedSid(pOwner)) and (not (bKeyStore and SidIs(pOwner, HM_SERVICE_SID))) then
                     begin
                        if (not bCode) and (not bFolder) and SidIs(pOwner, HM_SERVICE_SID) then
                           szWho := ''
                        else
                           szWho := 'is owned by ' + SidNameAt(pOwner) + ', an account other than SYSTEM, the Administrators or TrustedInstaller, which may change it whatever its permissions say';
                     end
                     else if bCode or bFolder then
                     begin
                        if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, bKeyStore, szText) then
                        begin
                           szWho := 'has an entry of its own that lets ' + szText + ' change it';
                           szRemedy := 'An administrator checks it and gives it the permissions of the folder it is in - icacls "' + szItem + '" /reset - restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
                        end
                        else if (not Final) and StaleEntryLetsOthers(pDacl, pFolderDescriptor, pFolderDacl, bKeyStore, bFolder, szText) then
                           NoteStaleItem(szItem, szText);
                     end;
                     FreeWindowsMemory(pDescriptor);
                  end;
                  ReleaseFolder(handle);
               end;
               if (szWho <> '') and (szRemedy = '') then
               begin
                  if Pos('could not be read', szWho) > 0 then
                     szRemedy := 'An administrator takes it - takeown /F "' + szItem + '" /A - checks it, restarts Windows and runs setup again.'
                  else
                     szRemedy := 'An administrator checks that it is meant to be there and gives it to the Administrators - icacls "' + szItem + '" /setowner *S-1-5-32-544 - or removes it, restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
               end;
            end;

            if szWho <> '' then
            begin
               if Pos('is ', szWho) <> 1 then
                  if Pos('has ', szWho) <> 1 then
                     szWho := 'is there, and ' + szWho;
               szText := szItem + ' ' + szWho + '. ' + szRemedy;
               if bLink or bCode or bFolder then
               begin
                  Found := Found + 1;
                  if Found <= 10 then
                     Why := Why + szText + ' '
                  else if Found = 11 then
                     Why := Why + 'There are more; the setup log names each. ';
                  Log('The program folder: refused, ' + szText);
                  Result := false;
               end
               else if Final then
               begin
                  szText := szText + ' Setup replaces the files it installs; one it does not install stays as it is.';
                  Log('The program folder: ' + szText);
                  g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;
               end;
            end
            else if bFolder then
            begin
               // An empty folder there before this installation, in the last look:
               // removed, since a program that opened it while another account could
               // change it may make it a junction whatever its permissions say now, and
               // the look after the files holds Bin, ControlPanel and the folders that
               // hold what is run to having no empty folder setup did not leave
               // (NothingTurnedIn); Inno Setup makes it again if it installs into it.
               if Final and FolderHoldsNothing(szItem) then
               begin
                  if RemoveHeldFolder(szItem, szFinal, szRemoved) then
                     Log('Setup removed ' + szItem + ', an empty folder there before this installation; Inno Setup makes it again if it installs into it.')
                  else
                  begin
                     szText := szItem + ' is an empty folder there before this installation, and could not be removed - ' + szRemoved +
                               '. An administrator closes the program that holds it, or restarts Windows, and runs setup again.';
                     Found := Found + 1;
                     if Found <= 10 then
                        Why := Why + szText + ' '
                     else if Found = 11 then
                        Why := Why + 'There are more; the setup log names each. ';
                     Log('The program folder: refused, ' + szText);
                     Result := false;
                  end;
               end
               else
               begin
                  SetArrayLength(folders, count + 1);
                  folders[count] := szItem;
                  count := count + 1;
               end;
            end;
         end;
      until not FindNext(rec);
   finally
      FindClose(rec);
   end;
   if pFolderDescriptor <> 0 then
      FreeWindowsMemory(pFolderDescriptor);

   for index := 0 to count - 1 do
      if not CheckWhatIsInside(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Final, Scripts, Found, Why) then
         Result := false;
   ReleaseFolder(folderHandle);
end;

// Whether the volume a held folder is on keeps permissions (FILE_PERSISTENT_ACLS):
// not FAT or exFAT, which keep none, so that any account may change any file there.
// Read through the handle that holds the deepest folder of each walk
// (HoldFolderChain) - the program folder's, Bin's and ControlPanel's, each of which a
// link the administrators control may take to a volume of its own. False, with Why,
// when it keeps none or cannot be told: setup does not install where it cannot know.
function VolumeKeepsPermissions(HeldHandle: Integer; Folder: String; var Why: String): Boolean;
var
   serial: cardinal;
   longest: cardinal;
   flags: cardinal;
begin
   Result := false;
   Why := '';
   serial := 0;
   longest := 0;
   flags := 0;
   if not VolumeOfHeldFolder(HeldHandle, 0, 0, serial, longest, flags, 0, 0) then
      Why := 'whether the volume of ' + Folder + ' keeps permissions could not be read (Windows error ' + IntToStr(DLLGetLastError()) + '), so setup cannot know that anybody but the ' +
             'administrators is kept from changing the programs there'
   else if (flags and HM_PERSISTENT_ACLS) = 0 then
      Why := Folder + ' is on a volume that keeps no permissions (FAT or exFAT), where every account of this computer may change the programs that the service and the ' +
             'administrators run, and nothing can stop that'
   else
      Result := true;
end;

// Whether Path is Folder or in it, both final paths (FinalPathOfHeld, FinalPathOf);
// false when either is ''.
function IsAtOrBelow(Path: String; Folder: String): Boolean;
begin
   Result := false;
   if (Path = '') or (Folder = '') then
      Exit;
   Result := (CompareText(AddBackslash(Path), AddBackslash(Folder)) = 0) or (CompareText(Copy(Path, 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0);
end;

// The folder the profiles of the computer's users are in (ProfileList's
// ProfilesDirectory, C:\Users as Windows is installed), or '' when it cannot be read.
function ProfilesFolder(): String;
begin
   Result := '';
   if RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList', 'ProfilesDirectory', Result) then
      StringChangeEx(Result, '%SystemDrive%', GetEnv('SystemDrive'), true)
   else
      Result := '';
end;

// Work item 78's R4: whether /GrantServiceAccess would grant the service the folder a
// data folder's link leads to, held by TargetHandle (DataLinkLeadsWhereAllowed): it
// grants it nothing - ServiceAccount.md, "Never granted, approved or not" - in a
// drive's root (a volume mounted with no drive letter of its own is gone through, its
// root granted); in what is, holds or is in the Windows folder, a Program Files
// folder, the profiles folder or ProgramData\Microsoft; in what is or holds ProgramData;
// or in what is or holds the program folder, or is in it but outside its own Data,
// Logs, Temp and Database - Bin above all. Each is asked by the final paths of the two
// folders, read through handles (FinalPathOfHeld, FinalPathOf), so that no letter, mount
// point or link names one another way. Such an installation would pass setup's look
// and then fail its grant after the files, or go on under LocalSystem (check 4 of R1 to
// R4, 27 October 2026). False, with Why, otherwise.
function DataLinkTargetIsGrantable(TargetHandle: Integer; Target: String; var Why: String): Boolean;
var
   szFinal: String;
   szName: String;
   szOther: String;
   szNames: String;
   szApp: String;
   returned: cardinal;
   index: Integer;
   start: Integer;
begin
   Result := false;
   Why := '';
   szFinal := FinalPathOfHeld(TargetHandle);
   if szFinal = '' then
   begin
      Why := 'where it leads, ' + Target + ', could not be read through the handle that holds it (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   if IsVolumeRoot(szFinal) then
   begin
      SetLength(szNames, 2048);
      returned := 0;
      if not VolumePathNames(szFinal, szNames, 2048, returned) then
      begin
         Why := 'it leads to the root of a volume, ' + szFinal + ', whose drive letters could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')';
         Exit;
      end;
      start := 1;
      for index := 1 to returned do
         if (index > Length(szNames)) or (szNames[index] = #0) then
         begin
            szName := Copy(szNames, start, index - start);
            start := index + 1;
            // Each character read only where the name is long enough, as in SplitAtRoot.
            if Length(szName) = 3 then
               if szName[2] = ':' then
               begin
                  Why := 'it leads to ' + szName + ', the root of a drive, which /GrantServiceAccess never grants the service';
                  Exit;
               end;
         end;
   end;
   for index := 0 to 4 do
   begin
      case index of
         0: szName := ExpandConstant('{win}');
         1: szName := ExpandConstant('{commonpf64}');
         2: szName := ExpandConstant('{commonpf32}');
         3: szName := ProfilesFolder();
         4: szName := AddBackslash(ExpandConstant('{commonappdata}')) + 'Microsoft';
      end;
      szOther := '';
      if szName <> '' then
         szOther := FinalPathOf(szName);
      if IsAtOrBelow(szFinal, szOther) or IsAtOrBelow(szOther, szFinal) then
      begin
         Why := 'it leads to ' + Target + ', which is, holds or is in ' + szName + ', where /GrantServiceAccess never grants the service anything';
         Exit;
      end;
   end;
   szOther := FinalPathOf(ExpandConstant('{commonappdata}'));
   if IsAtOrBelow(szOther, szFinal) then
   begin
      Why := 'it leads to ' + Target + ', which is or holds ' + ExpandConstant('{commonappdata}') + ', which /GrantServiceAccess never grants the service';
      Exit;
   end;
   szApp := FinalPathOfHeld(g_hProgramFolder);
   if IsAtOrBelow(szApp, szFinal) then
   begin
      Why := 'it leads to ' + Target + ', which is or holds the program folder';
      Exit;
   end;
   if IsAtOrBelow(szFinal, szApp) then
      if not (IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Data') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Logs') or IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Temp') or
              IsAtOrBelow(szFinal, AddBackslash(szApp) + 'Database')) then
      begin
         Why := 'it leads to ' + Target + ', which is in the program folder outside its Data, Logs, Temp and Database - where Bin is - and which /GrantServiceAccess never grants ' +
                'the service';
         Exit;
      end;
   Result := true;
end;

// Whether a list of permissions has an allow entry that applies to what it is on and
// lets a group of every user (IsBroadSid) write it (HM_FOLDER_WRITE_RIGHTS), inherited
// or its own; an entry that cannot be read counts as one.
function AclLetsEveryUserWrite(pDacl: cardinal): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
begin
   Result := pDacl = 0;
   if Result then
      Exit;
   if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      Result := true;
      Exit;
   end;
   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         if ((((head.TypeFlagsSize shr 8) and $FF) and HM_INHERIT_ONLY) = 0) and LetsEveryUserWrite(pAce) then
            Result := true;
      end
      else
         Result := true;
      index := index + 1;
   end;
end;

// Work item 78's R4: the folder a Database link leads to, held by TargetHandle, and
// every file in it, held to the rule of a settings file (IsSettingsFile): the built-in
// database there holds, among its settings, the program the service runs to scan for
// viruses (AntiVirusConfiguration's CustomScannerExecutable), so whoever may change it
// may give the service a program to run. Its owner, and each file's, SYSTEM, the
// Administrators, TrustedInstaller or the service, which makes the database's files;
// no group of every user able to write it or one of them (AclLetsEveryUserWrite); and
// no link among them. False, with Why, otherwise.
function DatabaseTargetIsTheAdministrators(TargetHandle: Integer; Target: String; var Why: String): Boolean;
var
   rec: TFindRec;
   handle: Integer;
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   szItem: String;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   if ReadHeldSecurity(TargetHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then
   begin
      Why := 'the owner and permissions of ' + Target + ', where it leads, could not be read';
      Exit;
   end;
   if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then
      Why := 'it leads to ' + Target + ', whose owner is not SYSTEM, the Administrators, TrustedInstaller or the service, and which holds the database whose settings name ' +
             'the program the service runs to scan for viruses'
   else if AclLetsEveryUserWrite(pDacl) then
      Why := 'it leads to ' + Target + ', which a group of every user may change, and which holds the database whose settings name the program the service runs to scan for ' +
             'viruses';
   FreeWindowsMemory(pDescriptor);
   if Why <> '' then
      Exit;
   if FindFirst(AddBackslash(Target) + '*', rec) then
   begin
      try
         repeat
            if (Why = '') and ((rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then
            begin
               szItem := AddBackslash(Target) + rec.Name;
               if (rec.Attributes and HM_REPARSE_POINT) <> 0 then
                  Why := szItem + ', in the folder it leads to, is a link'
               else
               begin
                  handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
                  if handle = -1 then
                     Why := 'the owner of ' + szItem + ', in the folder it leads to, could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')'
                  else
                  begin
                     pOwner := 0;
                     pDacl := 0;
                     pDescriptor := 0;
                     if ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor) <> 0 then
                        Why := 'the owner of ' + szItem + ', in the folder it leads to, could not be read'
                     else
                     begin
                        if (pOwner = 0) or ((not IsTrustedSid(pOwner)) and (not SidIs(pOwner, HM_SERVICE_SID))) then
                           Why := szItem + ', in the folder it leads to, is owned by an account other than SYSTEM, the Administrators, TrustedInstaller or the service'
                        else if AclLetsEveryUserWrite(pDacl) then
                           Why := szItem + ', in the folder it leads to, may be changed by a group of every user';
                        FreeWindowsMemory(pDescriptor);
                     end;
                     ReleaseFolder(handle);
                  end;
               end;
            end;
         until (Why <> '') or (not FindNext(rec));
      finally
         FindClose(rec);
      end;
   end;
   Result := Why = '';
end;

// Work item 78's R4, for a data folder of the program folder's own level - Data, Logs,
// Temp or Database - that is a link only the administrators control: where it leads
// held to the rules of the program folder's own way (HoldFolderChain from the link's
// target, LinkTarget, read through a hold on the link, HoldForSetup): every link on the
// way to the rule of a link, every folder above it to the rule of a folder above the
// program folder, no share, no network drive, the folder there, and its volume one
// that keeps permissions (VolumeKeepsPermissions). Keeping Data or Logs on another
// drive behind an administrator's junction or mount point is an ordinary layout
// (docs/ServiceAccount.md), and the folder the link leads to may hand down what the
// root of that drive hands down, as the server's own store commands take it: what it
// inherits is not asked. It is held to the rule of a data folder in the program folder
// (DataFolderIsAllowed, check 5 of R1 to R4, 27 October 2026): its owner SYSTEM, the
// Administrators, TrustedInstaller or the service, and no entry of its own letting another
// account but the service write it - but for the earlier installation's that another
// account owns, which setup takes before a restart (AddRestartFolder, HardenForRestart),
// in the first look. False, with Why, otherwise.
function DataLinkLeadsWhereAllowed(Link: String; Final: Boolean; var Why: String): Boolean;
var
   hLink: Integer;
   hHeld: Integer;
   szTarget: String;
   szReal: String;
   szHeld: String;
   szWhy: String;
   szWho: String;
   szRemedy: String;
   bWhole: Boolean;
   bKnown: Boolean;
   bOwner: Boolean;
begin
   Result := false;
   Why := '';
   hLink := HoldForSetup(Link);
   if hLink = -1 then
   begin
      Why := 'it could not be held open while setup looks at it (Windows error ' + IntToStr(DLLGetLastError()) + ')';
      Exit;
   end;
   if not HeldIsLink(hLink, bKnown) then
   begin
      Why := 'it was changed while setup looked at it';
      Exit;
   end;
   if not LinkTarget(hLink, Link, szTarget, szWhy) then
   begin
      Why := szWhy;
      Exit;
   end;
   if not HoldFolderChain(szTarget, 1, szReal, szHeld, hHeld, bWhole, szWhy) then
   begin
      Why := 'where it leads, ' + szTarget + ', is not where setup lets a data folder be: ' + szWhy;
      Exit;
   end;
   if not bWhole then
   begin
      Why := 'it leads to ' + szTarget + ', which is not there';
      Exit;
   end;
   if not VolumeKeepsPermissions(hHeld, szHeld, szWhy) then
   begin
      Why := 'where it leads, ' + szTarget + ', is not where setup lets a data folder be: ' + szWhy;
      Exit;
   end;
   // Where the grant after the files would refuse it, and, for the database, what the
   // service runs (check 4 of R1 to R4, 27 October 2026).
   if not DataLinkTargetIsGrantable(hHeld, szReal, szWhy) then
   begin
      Why := szWhy;
      Exit;
   end;
   if CompareText(ExtractFileName(Link), 'Database') = 0 then
      if not DatabaseTargetIsTheAdministrators(hHeld, szReal, szWhy) then
      begin
         Why := szWhy;
         Exit;
      end;
   if not DataFolderIsAllowed(szReal, hHeld, bOwner, szWho, szRemedy) then
   begin
      if bOwner and g_bEarlierInstallation and (not Final) then
         AddRestartFolder(szReal, hHeld, true, false, Link + ' led to ' + szReal + ', which was ' + Copy(szWho, 4, Length(szWho)))
      else
      begin
         Why := 'it leads to ' + szReal + ', which ' + szWho + ' (' + Copy(szRemedy, 1, Length(szRemedy) - 1) + ')';
         Exit;
      end;
   end;
   Result := true;
end;

// Work item 78's R4, past Bin and ControlPanel: what else in a program folder that is
// there holds what the service and the administrators run. Each item of the program
// folder's own level but Bin and ControlPanel, which have rules of their own - the
// uninstaller, a folder setup installs into, a data folder - read through a handle
// opened on it as itself (AppHandle holds the program folder):
//   - a link there that anybody but SYSTEM, the Administrators and TrustedInstaller owns
//     or may change (LinkIsAdministratorsAlone, the link itself: the program folder it
//     is in is setup's to harden), refused with the icacls that makes it theirs alone;
//     one they alone control refused too when it is Addons, Events, DBScripts, WebAdmin
//     or Languages (IsRunFolderName), which setup installs into and the service and the
//     administrators run from; and, when it is Data, Logs, Temp or Database - a data
//     folder behind an administrator's junction is an ordinary layout - held to where it
//     leads (DataLinkLeadsWhereAllowed);
//   - a program, a library, a script (IsScriptFile), a file that says what they run
//     (IsSettingsFile: unins000.dat, the uninstaller's record) or a folder whose owner is not one
//     of the three, or cannot be read, or an entry of whose own lets another account
//     change it, refused; and, in the first look, one of the first four that an entry it
//     inherited where it was made, and the program folder does not hand down, lets another
//     account change noted for the restart rule (StaleEntryLetsOthers, NoteStaleItem);
//   - Data, Logs, Temp and Database (IsDataFolderName), which the service may own and has
//     an entry of its own on (/GrantServiceAccess), held to the rule of a data folder
//     (DataFolderIsAllowed): one whose owner is not one of the three or the service, or an
//     entry of whose own lets an account other than those write it, refused - but for the
//     earlier installation's that another account owns, which setup takes before a
//     restart, in the first look (AddRestartFolder, HardenForRestart) - and the files of
//     the Database held to their rule too (DatabaseFilesWhy): /ProtectFolders, after the
//     files, keeps a data folder's own entries and its owner, and so would leave that
//     account the message store, the logs, the messages being scanned or the built-in
//     database (check 5 of R1 to R4, 27 October 2026);
// and then everything, at any depth, in Addons, Events, DBScripts, WebAdmin and
// Languages, the folders setup installs into that hold programs, the event script
// the service runs, the SQL DBSetup runs and the PHP of WebAdmin, held to
// CheckWhatIsInside's rule with scripts counted as programs. A program folder that is
// the root of a volume (IsVolumeRoot), which CheckProgramFolders refuses before this
// runs, is not looked at on its own level. Found counts
// what was refused; what is said is said when Final is set. In the last look (Final)
// an empty folder there before this installation that setup or Inno Setup makes again -
// one of the five, and a data folder when the server is installed, which [Dirs] makes -
// is removed through a handle opened on it as itself (RemoveHeldFolder): one a program
// opened while another account could change it may be made a junction whatever its
// permissions say now, and setup would install, and the service write its messages or
// its logs, where that leads. Any other empty folder there, which nothing installs into
// or runs from, is left as it is, and said in the setup log (check 5 of R1 to R4). The
// five folders are walked only as the folders of their names in the program folder
// (CheckWhatIsInside, Expected).
function CheckWhatElseIsInside(App: String; AppHandle: Integer; Final: Boolean; var Found: Integer; var Why: String): Boolean;
var
   rec: TFindRec;
   index: Integer;
   handle: Integer;
   szAppFinal: String;
   szRemoved: String;
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   bFolder: Boolean;
   bLink: Boolean;
   bData: Boolean;
   bCode: Boolean;
   szItem: String;
   szWho: String;
   szText: String;
   szRemedy: String;
   szFolder: String;
   bOwner: Boolean;
   pAppOwner: cardinal;
   pAppDacl: cardinal;
   pAppDescriptor: cardinal;
begin
   Result := true;
   szAppFinal := FinalPathOfHeld(AppHandle);
   if not IsVolumeRoot(App) then
   begin
      if not FindFirst(AddBackslash(App) + '*', rec) then
      begin
         Found := Found + 1;
         Why := Why + 'what is in ' + App + ' could not be read (Windows error ' + IntToStr(DLLGetLastError()) + '). ';
         Result := false;
         Exit;
      end;
      // What the program folder hands down to the files in it (StaleEntryLetsOthers);
      // pAppDescriptor 0 when it cannot be read.
      pAppOwner := 0;
      pAppDacl := 0;
      pAppDescriptor := 0;
      if ReadHeldSecurity(AppHandle, HM_FILE_OBJECT, HM_DACL_INFORMATION, pAppOwner, 0, pAppDacl, 0, pAppDescriptor) <> 0 then
         pAppDescriptor := 0;
      try
         repeat
            if (rec.Name <> '.') and (rec.Name <> '..') and (CompareText(rec.Name, 'Bin') <> 0) and (CompareText(rec.Name, 'ControlPanel') <> 0) then
            begin
               szItem := AddBackslash(App) + rec.Name;
               bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
               bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;
               bData := bFolder and IsDataFolderName(rec.Name);
               bCode := (not bFolder) and (IsProgramFile(rec.Name) or IsScriptFile(rec.Name) or IsSettingsFile(rec.Name));
               szWho := '';
               szRemedy := '';
               handle := HoldFolder(szItem, HM_READ_CONTROL, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
               if handle = -1 then
                  szWho := 'its owner could not be read (Windows error ' + IntToStr(DLLGetLastError()) + ')'
               else
               begin
                  if bLink then
                  begin
                     if not LinkIsAdministratorsAlone(handle, -1, szText) then
                     begin
                        szWho := 'is a junction, a symbolic link or a mount point, and ' + szText;
                        szRemedy := LinkRemedy(szItem) + '.';
                     end
                     else if bFolder and IsRunFolderName(rec.Name) then
                     begin
                        szWho := 'is a junction, a symbolic link or a mount point, and setup installs into it what the service or the administrators run, which would then be ' +
                                 'wherever the link leads, in a folder setup does not prepare';
                        szRemedy := 'An administrator removes the link - rmdir "' + szItem + '", which leaves what it leads to as it is - and runs setup again, which makes the ' +
                                    'folder itself. Event scripts kept in another folder are named by EventFolder in Bin\hMailServer.INI, as RelocatingAnInstallation.md says.';
                     end
                     else if bData and (not DataLinkLeadsWhereAllowed(szItem, Final, szText)) then
                     begin
                        szWho := 'is a junction, a symbolic link or a mount point that only the administrators control, but ' + szText;
                        szRemedy := 'Setup holds where a data folder''s link leads, and the way there, to the rules of the program folder''s own way: an administrator sees to ' +
                                    'what is named, or points the link at a folder that passes them, and runs setup again.';
                     end;
                  end
                  else if bData then
                  begin
                     // A data folder, held to its rule (DataFolderIsAllowed); the earlier
                     // installation's that another account owns noted, in the first look, for
                     // HardenForRestart, which takes it before a restart; and the Database's
                     // files held to theirs (DatabaseFilesWhy).
                     if not DataFolderIsAllowed(szItem, handle, bOwner, szWho, szRemedy) then
                     begin
                        if bOwner and g_bEarlierInstallation and (not Final) then
                        begin
                           AddRestartFolder(szItem, -1, true, false, szItem + ' was ' + Copy(szWho, 4, Length(szWho)));
                           szWho := '';
                           szRemedy := '';
                        end;
                     end
                     else if CompareText(rec.Name, 'Database') = 0 then
                        szWho := DatabaseFilesWhy(szItem, szRemedy);
                  end
                  else
                  begin
                     pOwner := 0;
                     pDacl := 0;
                     pDescriptor := 0;
                     readResult := ReadHeldSecurity(handle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
                     if readResult <> 0 then
                        szWho := 'its owner could not be read (Windows error ' + IntToStr(readResult) + ')'
                     else
                     begin
                        if pOwner = 0 then
                           szWho := 'its owner could not be read'
                        else if not IsTrustedSid(pOwner) then
                           szWho := 'is owned by ' + SidNameAt(pOwner) + ', an account other than SYSTEM, the Administrators or TrustedInstaller, which may change it whatever its permissions say'
                        else if bCode or bFolder then
                        begin
                           if OwnEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, false, szText) then
                           begin
                              szWho := 'has an entry of its own that lets ' + szText + ' change it';
                              szRemedy := 'An administrator checks it and gives it the permissions of the folder it is in - icacls "' + szItem + '" /reset - restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
                           end
                           else if bCode and (not Final) and StaleEntryLetsOthers(pDacl, pAppDescriptor, pAppDacl, false, false, szText) then
                              NoteStaleItem(szItem, szText);
                        end;
                        FreeWindowsMemory(pDescriptor);
                     end;
                  end;
                  ReleaseFolder(handle);
               end;
               if (szWho <> '') and (szRemedy = '') then
               begin
                  if Pos('could not be read', szWho) > 0 then
                     szRemedy := 'An administrator takes it - takeown /F "' + szItem + '" /A - checks it, restarts Windows and runs setup again.'
                  else
                     szRemedy := 'An administrator checks that it is meant to be there and gives it to the Administrators - icacls "' + szItem + '" /setowner *S-1-5-32-544 - or removes it, restarts Windows, since a program that account started may still hold it open with the right to change it, and runs setup again.';
               end;

               if szWho <> '' then
               begin
                  if Pos('is ', szWho) <> 1 then
                     if Pos('has ', szWho) <> 1 then
                        szWho := 'is there, and ' + szWho;
                  szText := szItem + ' ' + szWho + '. ' + szRemedy;
                  if bLink or bCode or bFolder then
                  begin
                     Found := Found + 1;
                     if Found <= 10 then
                        Why := Why + szText + ' '
                     else if Found = 11 then
                        Why := Why + 'There are more; the setup log names each. ';
                     Log('The program folder: refused, ' + szText);
                     Result := false;
                  end
                  else if Final then
                  begin
                     szText := szText + ' Setup replaces the files it installs; one it does not install stays as it is.';
                     Log('The program folder: ' + szText);
                     g_szFolderFindings := g_szFolderFindings + szText + #13#10#13#10;
                  end;
               end
               else if Final and bFolder and (not bLink) and (not IsRunFolderName(rec.Name)) and (not (bData and IsComponentSelected('server'))) and
                       FolderHoldsNothing(szItem) then
                  // An empty folder setup neither installs into nor makes again: nothing is
                  // installed into it or run from it, wherever a link made of it led, and an
                  // administrator's folder is not setup's to remove (check 5 of R1 to R4).
                  Log('The program folder: ' + szItem + ' is an empty folder there before this installation, which setup neither installs into nor makes again, and is left ' +
                      'as it is.')
               else if Final and bFolder and (not bLink) and FolderHoldsNothing(szItem) then
               begin
                  // An empty folder, there before this installation, of those setup
                  // installs into, or a data folder when the server is installed: removed,
                  // as in Bin; [Dirs] makes it again (a data folder only with the server).
                  if RemoveHeldFolder(szItem, szAppFinal, szRemoved) then
                     Log('Setup removed ' + szItem + ', an empty folder there before this installation; Inno Setup makes it again if it installs into it.')
                  else
                  begin
                     szText := szItem + ' is an empty folder there before this installation, and could not be removed - ' + szRemoved +
                               '. An administrator closes the program that holds it, or restarts Windows, and runs setup again.';
                     Found := Found + 1;
                     if Found <= 10 then
                        Why := Why + szText + ' '
                     else if Found = 11 then
                        Why := Why + 'There are more; the setup log names each. ';
                     Log('The program folder: refused, ' + szText);
                     Result := false;
                  end;
               end;
            end;
         until not FindNext(rec);
      finally
         FindClose(rec);
      end;
      if pAppDescriptor <> 0 then
         FreeWindowsMemory(pAppDescriptor);
   end;

   // The folders setup installs into that hold what is run, at any depth.
   for index := 0 to 4 do
   begin
      case index of
         0: szFolder := 'Addons';
         1: szFolder := 'Events';
         2: szFolder := 'DBScripts';
         3: szFolder := 'WebAdmin';
         4: szFolder := 'Languages';
      end;
      szFolder := AddBackslash(App) + szFolder;
      // One that is a link is refused above, and not looked through.
      if FolderAttributes(szFolder) <> HM_INVALID_ATTRIBUTES then
         if ((FolderAttributes(szFolder) and FILE_ATTRIBUTE_DIRECTORY) <> 0) and ((FolderAttributes(szFolder) and HM_REPARSE_POINT) = 0) then
            if not CheckWhatIsInside(szFolder, AddBackslash(szAppFinal) + ExtractFileName(szFolder), 0, Final, true, Found, Why) then
               Result := false;
   end;
end;

// The first of the data folders hMailServer.exe /ProtectFolders makes private - Data,
// Logs, Temp, Database, Events - that is there in App and still inherits from it, or
// '' when none does.
function DataFolderThatInherits(App: String): String;
var
   index: Integer;
   szFolder: String;
begin
   Result := '';
   for index := 0 to 4 do
   begin
      case index of
         0: szFolder := 'Data';
         1: szFolder := 'Logs';
         2: szFolder := 'Temp';
         3: szFolder := 'Database';
         4: szFolder := 'Events';
      end;
      szFolder := AddBackslash(App) + szFolder;
      if (Result = '') and DirExists(szFolder) and (not InheritsNothing(szFolder)) then
         Result := szFolder;
   end;
end;

// Whether the walks of Bin and ControlPanel (Held, Whole) agree with the program
// folder's: what was there for one is there for the others, and nothing that was not
// there for the first is there for a later one - a folder or a link made in the
// meantime is refused, not looked at by its path.
function WalksAgree(Held: String; Whole: Boolean): Boolean;
begin
   if g_bProgramFolderThere then
      Result := Whole or (CompareText(Held, g_szProgramFolderReal) = 0)
   else
      Result := (not Whole) and (CompareText(Held, g_szProgramFolderHeld) = 0);
end;

// Whether this installation installs the Control Panel, whose folder setup makes when
// the program folder is there and the folder is not.
function ControlPanelIsInstalled(): Boolean;
begin
   Result := IsComponentSelected('controlpanel');
end;

// The owner of the program folder, Bin or ControlPanel, there before this
// installation, read through the handle that holds it: SYSTEM, the Administrators or
// TrustedInstaller, or any other only for the earlier installation's folder
// (g_bEarlierInstallation), which setup gives to the Administrators and then installs
// into only after Windows has restarted (HardenForRestart). False, with Why, otherwise.
function OwnerIsAllowed(Folder: String; HeldHandle: Integer; var Why: String): Boolean;
var
   bKnown: Boolean;
   szWho: String;
begin
   Result := HeldOwnerIsTrusted(HeldHandle, bKnown, szWho);
   Why := '';
   if Result then
      Exit;
   if not bKnown then
      Why := 'the owner of ' + Folder + ' could not be read (' + szWho + '); an administrator takes it - takeown /F "' + Folder + '" /A - restarts Windows and runs setup again'
   else if g_bEarlierInstallation then
      Result := true
   else
      Why := Folder + ' was made by ' + szWho + ', who may still hold it open with the right to change its permissions, so setup does not take it: remove or rename it and run ' +
             'setup again, which makes a new one with permissions of its own, or install into another folder';
end;

// Where the walk of the program folder (Which 0), Bin (1) or ControlPanel (2) ended, by the
// path its held handle gives (FinalPathOfHeld, \\?\Volume{...}\...), with the names not
// there yet after it, whatever drive letter, mount point, link or short name {app} reached
// it by; '' when it was not walked or cannot be read (check 5 of R1 to R4, 27 October 2026).
function WalkedFinal(Which: Integer): String;
var
   handle: Integer;
   szHeld: String;
   szReal: String;
   szFinal: String;
begin
   Result := '';
   case Which of
      0: begin handle := g_hProgramFolder; szHeld := g_szProgramFolderHeld; szReal := g_szProgramFolderReal; end;
      1: begin handle := g_hBin; szHeld := g_szBinHeld; szReal := g_szBinReal; end;
   else
      begin handle := g_hControlPanel; szHeld := g_szControlPanelHeld; szReal := g_szControlPanelReal; end;
   end;
   if (handle = -1) or (szHeld = '') or (szReal = '') then
      Exit;
   szFinal := FinalPathOfHeld(handle);
   if szFinal = '' then
      Exit;
   if CompareText(szHeld, szReal) = 0 then
      Result := szFinal
   else if CompareText(Copy(szReal, 1, Length(AddBackslash(szHeld))), AddBackslash(szHeld)) = 0 then
      Result := AddBackslash(szFinal) + Copy(szReal, Length(AddBackslash(szHeld)) + 1, Length(szReal));
end;

// Work item 78's R2 and R3: whether setup has recorded, since Windows last started,
// that it is not to install into App until Windows has restarted (RecordRestartNeeded),
// and what it recorded. The key is volatile: Windows does not keep it past a restart.
function RestartNeededFor(App: String; var What: String): Boolean;
begin
   What := '';
   Result := RegQueryStringValue(HKLM64, HM_RESTART_KEY, App, What);
end;

// Work item 78's R2 and R3 (check 5 of R1 to R4, 27 October 2026): whether setup has
// recorded, since Windows last started, a folder at or above where the walk of the program
// folder, Bin or ControlPanel ended (WalkedFinal) - by the path a held handle gave, as
// RecordRestartNeeded writes it beside {app} - so that another name for the same folder, an
// administrators' junction or mount point to it, a short name or a second drive letter of
// its volume, finds the record too; and what it recorded. Asked once the walks are done,
// which change nothing.
function RestartRecordedForWalked(var What: String): Boolean;
var
   names: TArrayOfString;
   index: Integer;
   which: Integer;
   szWalked: String;
begin
   Result := false;
   What := '';
   if not RegGetValueNames(HKLM64, HM_RESTART_KEY, names) then
      Exit;
   for index := 0 to GetArrayLength(names) - 1 do
      if (not Result) and (CompareText(Copy(names[index], 1, 4), '\\?\') = 0) then
         for which := 0 to 2 do
         begin
            szWalked := WalkedFinal(which);
            if (not Result) and IsAtOrBelow(szWalked, names[index]) then
            begin
               Result := true;
               if not RegQueryStringValue(HKLM64, HM_RESTART_KEY, names[index], What) then
                  What := names[index];
            end;
         end;
end;

// Work item 78's R2 and R3: that setup is not to install into App until Windows has
// restarted, recorded in a volatile key of HKEY_LOCAL_MACHINE, which only an
// administrator may write and which Windows does not keep past a restart - after setup
// hardened a folder another account could change (HardenForRestart), and after a
// refusal whose remedy ends in a restart (PrepareToInstall, CurStepChanged): a program
// another account started while it could change a folder or a file there keeps a
// handle that keeps that right whatever the permissions say after, and a restart is the
// one thing that closes it. What says what setup did, for the refusal of a later run.
// Recorded by App, as {app} names it, and by where the walks of the program folder, Bin and
// ControlPanel ended (WalkedFinal), which a later run looks up once its own walks are done
// (RestartRecordedForWalked): another name for the same folder finds it (check 5 of R1 to
// R4, 27 October 2026).
procedure RecordRestartNeeded(App: String; What: String);
var
   key: cardinal;
   disposition: cardinal;
   written: Integer;
   which: Integer;
   szText: String;
   szFinal: String;
begin
   key := 0;
   disposition := 0;
   szText := GetDateTimeString('yyyy-mm-dd hh:nn:ss', '-', ':') + ': ' + What;
   written := MakeVolatileKey(HM_HKEY_LOCAL_MACHINE, HM_RESTART_KEY, 0, 0, HM_REG_OPTION_VOLATILE, HM_KEY_SET_VALUE_64, 0, key, disposition);
   if written = 0 then
   begin
      written := WriteKeyText(key, App, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2);
      for which := 0 to 2 do
      begin
         szFinal := WalkedFinal(which);
         if (written = 0) and (szFinal <> '') and (not IsVolumeRoot(szFinal)) then
            written := WriteKeyText(key, szFinal, 0, HM_REG_SZ, szText, (Length(szText) + 1) * 2);
      end;
      CloseKey(key);
   end;
   if written = 0 then
      Log('Setup has recorded until Windows restarts (HKLM\' + HM_RESTART_KEY + ', a volatile key) that it ' + What + ', and does not install into ' + App + ' before then.')
   else
      Log('Setup could not record that it ' + What + ' (Windows error ' + IntToStr(written) + '): an administrator restarts Windows before setup is run again.');
end;

// Work item 78's restart rule: whether an allow entry of a folder's list that applies to it,
// and that setup's hardening takes away - an inherited one, or one of its own for a group of
// every user (IsBroadSid, LetsEveryUserWrite) - lets an account other than SYSTEM, the
// Administrators or TrustedInstaller do any of Rights; OWNER RIGHTS is the owner's, held to
// the three before this is asked. An entry of its own for a named account or group setup
// keeps, as hMailServer.exe /ProtectFolders keeps it, and says (SayWriteEntriesKept): that
// account may change the folder after a restart as before it, and counting it refused every
// run of setup, a restart between them or not (check 5 of R1 to R4, 27 October 2026). An
// entry of a form setup does not read, or one that cannot be read, counts. No list at all
// lets everyone. Who is that account.
function TakenEntriesLetOthers(pDacl: cardinal; Rights: cardinal; var Who: String): Boolean;
var
   info: ACL_SIZE_INFO;
   head: ACE_START;
   pAce: cardinal;
   index: cardinal;
   aceType: cardinal;
   aceFlags: cardinal;
begin
   Result := false;
   Who := '';
   if pDacl = 0 then
   begin
      Result := true;
      Who := 'everyone (it has no list of permissions at all)';
      Exit;
   end;
   if not ReadAclSize(pDacl, info, 12, HM_ACL_SIZE_INFORMATION) then
   begin
      Result := true;
      Who := 'an account in a list of permissions that could not be read';
      Exit;
   end;
   index := 0;
   while (not Result) and (index < info.AceCount) do
   begin
      pAce := 0;
      if ReadAclEntry(pDacl, index, pAce) and (pAce <> 0) then
      begin
         CopyAceStart(head, pAce, 8);
         aceType := head.TypeFlagsSize and $FF;
         aceFlags := (head.TypeFlagsSize shr 8) and $FF;
         if (aceFlags and HM_INHERIT_ONLY) = 0 then
         begin
            if aceType = HM_ACCESS_ALLOWED then
            begin
               if ((head.Mask and Rights) <> 0) and (not IsTrustedSid(pAce + 8)) and (not SidIs(pAce + 8, 'S-1-3-4')) and
                  (((aceFlags and HM_INHERITED_ACE) <> 0) or IsBroadSid(pAce + 8)) then
               begin
                  Result := true;
                  Who := SidNameAt(pAce + 8);
               end;
            end
            else if (aceType = 4) or (aceType = 5) or (aceType = 9) or (aceType = 11) then
            begin
               Result := true;
               Who := 'an account in an entry of a form setup does not read';
            end;
         end;
      end
      else
      begin
         Result := true;
         Who := 'an account in an entry that could not be read';
      end;
      index := index + 1;
   end;
end;

// Work item 78's R2 and R3: whether another account could change a folder of the
// program that is there, before setup changes anything (CheckProgramFolders), read
// through a handle opened on it as itself: its owner not SYSTEM, the Administrators or
// TrustedInstaller (an earlier installation's, OwnerIsAllowed), who may always change
// its permissions; an allow entry that setup's hardening takes away, inherited or a group
// of every user's own (TakenEntriesLetOthers), that lets another account change its
// permissions or its owner (HM_CHANGE_PERMISSIONS_RIGHTS: Everyone's or Authenticated
// Users' Full control on a folder made beforehand, say); or, when Contents is set - the
// folder holds something but empty folders (HoldsOnlyEmptyFolders) - one that lets
// another account write it (HM_FOLDER_WRITE_RIGHTS). A program such an
// account started then keeps a handle, opened with that right, that setup's hardening
// does not take away (check 4 of R1 to R4, 27 October 2026): with the right to change
// the permissions it gives itself the folder back, and with a right to write a folder
// that holds something - Bin\hMailServer.INI, which Inno Setup edits in place, the key
// store, the uninstaller's record, which Inno Setup appends to, a library an
// administrator put there - it changes, or removes, what setup does not replace. What
// says which, and who; also when what it is cannot be read, which counts as one.
// Distrust is set for a folder SYSTEM, the Administrators or TrustedInstaller own whose
// entry setup's hardening takes away let another account change its permissions or its
// owner: that account could have written an entry of the folder's own for itself, which
// setup cannot tell from one an administrator gave, and so HardenForRestart keeps none
// that lets another account write (HardenDistrustedFolder, check 6 of R1 to R4, 28 October
// 2026); one another account owns HardenHeldFolder rebuilds so already.
function OthersCouldChange(Folder: String; HeldHandle: Integer; Contents: Boolean; var Distrust: Boolean; var What: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
   szWho: String;
begin
   Result := true;
   Distrust := false;
   What := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      What := 'the owner and permissions of ' + Folder + ' could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if pOwner = 0 then
      What := Folder + ' had no owner'
   else if not IsTrustedSid(pOwner) then
      What := Folder + ' was owned by ' + SidNameAt(pOwner) + ', who may always change its permissions'
   else if TakenEntriesLetOthers(pDacl, HM_CHANGE_PERMISSIONS_RIGHTS, szWho) then
   begin
      What := Folder + ' let ' + szWho + ' change its permissions or its owner';
      Distrust := true;
   end
   else if Contents and TakenEntriesLetOthers(pDacl, HM_FOLDER_WRITE_RIGHTS, szWho) then
      What := Folder + ' let ' + szWho + ' change what is in it'
   else
      Result := false;
   FreeWindowsMemory(pDescriptor);
end;

// What NoteStaleItem found in Folder - directly in it for the program folder, at any depth
// for Bin, ControlPanel and the five that hold what is run - or ''.
function StaleItemIn(Folder: String): String;
var
   index: Integer;
   bDirect: Boolean;
   bIn: Boolean;
begin
   Result := '';
   bDirect := CompareText(Folder, g_szProgramFolderReal) = 0;
   for index := 0 to GetArrayLength(g_aStaleItems) - 1 do
   begin
      if bDirect then
         bIn := CompareText(ExtractFileDir(g_aStaleItems[index]), Folder) = 0
      else
         bIn := CompareText(Copy(g_aStaleItems[index], 1, Length(AddBackslash(Folder))), AddBackslash(Folder)) = 0;
      if bIn and (Result = '') then
         Result := g_aStaleWhat[index];
   end;
end;

// One folder of the program that is there noted for HardenForRestart (AddRestartFolder)
// when another account could change it (OthersCouldChange), or a program, a library, a
// script or a settings file in it by an entry it kept from where it was made (StaleItemIn,
// check 5 of R1 to R4, 27 October 2026) - hardening the folder rewrites that entry, and a
// restart closes what was opened while it applied - with the handle that holds it when
// Keep is set (the walk's, for the program folder, Bin and ControlPanel), or -1 for one
// HardenForRestart holds itself (Addons, Events, DBScripts, WebAdmin, Languages). A right
// to write the folder counts when it holds something but empty folders, or when
// Unhardened is set: the program folder PrepareProgramFolders does not harden before the
// files (ProgramFolderWaitsForTheFiles), into which Inno Setup makes the folders the last
// look removed and copies what they hold (check 6 of R1 to R4, 28 October 2026).
procedure NoteFolderOthersCouldChange(Folder: String; HeldHandle: Integer; Keep: Boolean; Unhardened: Boolean);
var
   bDistrust: Boolean;
   szWhat: String;
begin
   if not OthersCouldChange(Folder, HeldHandle, Unhardened or (not HoldsOnlyEmptyFolders(Folder)), bDistrust, szWhat) then
   begin
      szWhat := StaleItemIn(Folder);
      if szWhat = '' then
         Exit;
   end;
   if Keep then
      AddRestartFolder(Folder, HeldHandle, false, bDistrust, szWhat)
   else
      AddRestartFolder(Folder, -1, false, bDistrust, szWhat);
end;

// The data folder of the earlier installation's server - Data, Logs, Temp, Database or
// Events, the first that is there and still inherits from the program folder
// (DataFolderThatInherits) - for which PrepareProgramFolders does not harden the program
// folder before the files, its change being handed down through the message store; ''
// when it hardens it. Asked by CheckProgramFolders' NoteFoldersOthersCouldChange and by
// PrepareProgramFolders alike, so that a program folder left unhardened before the files
// is one no other account could write (check 6 of R1 to R4, 28 October 2026).
function ProgramFolderWaitsForTheFiles(): String;
begin
   Result := '';
   if g_bEarlierInstallation then
      if DirExists(AddBackslash(g_szProgramFolderReal) + 'Data') or FileExists(AddBackslash(g_szBinReal) + 'hMailServer.INI') then
         Result := DataFolderThatInherits(g_szProgramFolderReal);
end;

// Work item 78's R2 and R3, in CheckProgramFolders, which changes nothing: the folders
// of the program that are there and that another account could change, noted
// (NoteFolderOthersCouldChange) - the program folder, Bin and ControlPanel but one that
// setup makes again (g_bRemakeBin, g_bRemakeControlPanel), and Addons, Events,
// DBScripts, WebAdmin and Languages when each is there and no link (one that is a link
// CheckWhatElseIsInside refuses) - after the data folders CheckWhatElseIsInside noted;
// CheckProgramFolders empties the list when it starts. Nothing when the program folder
// is not there.
procedure NoteFoldersOthersCouldChange();
var
   index: Integer;
   handle: Integer;
   bKnown: Boolean;
   szFolder: String;
begin
   if not g_bProgramFolderThere then
      Exit;
   NoteFolderOthersCouldChange(g_szProgramFolderReal, g_hProgramFolder, true, g_szProgramFolderWaits <> '');
   if g_bBinThere and (not g_bRemakeBin) then
      NoteFolderOthersCouldChange(g_szBinReal, g_hBin, true, false);
   if g_bControlPanelThere and (not g_bRemakeControlPanel) then
      NoteFolderOthersCouldChange(g_szControlPanelReal, g_hControlPanel, true, false);
   for index := 0 to 4 do
   begin
      case index of
         0: szFolder := 'Addons';
         1: szFolder := 'Events';
         2: szFolder := 'DBScripts';
         3: szFolder := 'WebAdmin';
         4: szFolder := 'Languages';
      end;
      szFolder := AddBackslash(g_szProgramFolderReal) + szFolder;
      handle := HoldFolder(szFolder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_EVERYTHING, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
      if handle <> -1 then
      begin
         if not HeldIsLink(handle, bKnown) then
            NoteFolderOthersCouldChange(szFolder, handle, false, false);
         ReleaseFolder(handle);
      end;
   end;
end;

// Work item 78's restart rule (check 6 of R1 to R4, 28 October 2026): a folder of the
// program whose owner, or an entry setup's hardening takes away, let another account
// change its permissions or its owner (OthersCouldChange's Distrust) - Everyone's Full
// control handed down to a folder made beforehand, say. Through that right the account
// could have written an entry of the folder's own for itself, which setup cannot tell from
// one an administrator gave, and which HardenHeldFolder would keep, as it keeps a named
// account's on a folder nobody else could change: so the folder is rebuilt as the earlier
// installation's folder another account owned is (RebuildFolderOfUntrustedOwner) -
// given to the Administrators with takeown.exe /A, which leaves one they own as it is,
// every entry of its own that lets an account other than SYSTEM, the Administrators or
// TrustedInstaller write dropped and named in the setup log, the entries that give no
// right to write kept (the service's Read & execute on Bin among them), SYSTEM and the
// Administrators full control, Users Read & execute where every user could read it, and
// nothing inherited - and read again through the handle that holds it, no account but the
// three left with a right to write (HeldFolderIsHardened). A folder another account owns
// that is not the earlier installation's is not taken (CheckProgramFolders refuses the
// program folder, Bin and ControlPanel of that kind before this). True when it was
// rebuilt; false with Why otherwise.
function HardenDistrustedFolder(Folder: String; HeldHandle: Integer; var Why: String): Boolean;
var
   pOwner: cardinal;
   pDacl: cardinal;
   pDescriptor: cardinal;
   readResult: cardinal;
begin
   Result := false;
   Why := '';
   pOwner := 0;
   pDacl := 0;
   pDescriptor := 0;
   readResult := ReadHeldSecurity(HeldHandle, HM_FILE_OBJECT, HM_OWNER_AND_DACL_INFORMATION, pOwner, 0, pDacl, 0, pDescriptor);
   if readResult <> 0 then
   begin
      Why := 'its owner and permissions could not be read (Windows error ' + IntToStr(readResult) + ')';
      Exit;
   end;
   if pOwner = 0 then
      Why := 'its owner could not be read'
   else if (not IsTrustedSid(pOwner)) and (not g_bEarlierInstallation) then
      Why := 'it was made by ' + SidNameAt(pOwner) + ', who may still hold it open with the right to change its permissions, and so it is not taken'
   else
      Result := RebuildFolderOfUntrustedOwner(Folder, pDacl, EveryUserMayRead(pDacl), false, Why);
   FreeWindowsMemory(pDescriptor);
   if not Result then
      Exit;
   Result := HeldFolderIsHardened(HeldHandle, true, false, Why);
   if not Result then
      Why := 'once setup had rebuilt its permissions, ' + Why;
end;

// Work item 78's R2 and R3: the folders of the program that were there and that
// another account could change (NoteFoldersOthersCouldChange) - each hardened, by
// HardenHeldFolder, as PrepareProgramFolders hardens one, or rebuilt by
// HardenDistrustedFolder when another account could change its permissions or its
// owner (check 6 of R1 to R4, 28 October 2026): the earlier installation's
// folder another account owned given to the Administrators and its permissions rebuilt,
// and the program folder hardened whatever its data folders inherit, its permissions
// handed down through them as hMailServer.exe /ProtectFolders hands them down after the
// files - and then the installation refused until Windows has restarted, recorded so
// (RecordRestartNeeded): a handle such an account opened while it could change one
// keeps that right after any change of the permissions (check 4 of R1 to R4, 27 October
// 2026), and only a restart closes it. Run before the service is stopped
// (CheckFoldersBeforeTheStop), so that the server goes on running until then. '' when
// there is none; otherwise the refusal, which PrepareToInstall gives (exit code 7 when
// silent).
function HardenForRestart(): String;
var
   index: Integer;
   handle: Integer;
   szApp: String;
   szWhy: String;
   szDone: String;
begin
   Result := '';
   if GetArrayLength(g_aRestartFolders) = 0 then
      Exit;
   szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
   RecordRestartNeeded(szApp, 'hardened folders of the program there that another account could change (work item 78)');
   szDone := '';
   for index := 0 to GetArrayLength(g_aRestartFolders) - 1 do
   begin
      szWhy := '';
      handle := g_aRestartHandles[index];
      if handle = -1 then
         handle := HoldForSetup(g_aRestartFolders[index]);
      if handle = -1 then
         szWhy := 'it could not be held open (Windows error ' + IntToStr(DLLGetLastError()) + ')'
      else if g_aRestartData[index] then
      begin
         if not HardenDataFolder(g_aRestartFolders[index], handle, szWhy) then
         begin
            if szWhy = '' then
               szWhy := 'its permissions could not be rebuilt';
         end
         else
            szWhy := '';
      end
      else if g_aRestartDistrust[index] then
      begin
         if not HardenDistrustedFolder(g_aRestartFolders[index], handle, szWhy) then
         begin
            if szWhy = '' then
               szWhy := 'its permissions could not be rebuilt';
         end
         else
            szWhy := '';
      end
      else if not HardenHeldFolder(g_aRestartFolders[index], handle, g_bEarlierInstallation, szWhy) then
      begin
         if szWhy = '' then
            szWhy := 'icacls did not harden it';
      end
      else
         szWhy := '';
      if szWhy <> '' then
      begin
         Result := 'Setup does not install into ' + szApp + ': before this installation, ' + g_szRestartWhy + ', and ' + g_aRestartFolders[index] + ' could not be hardened - ' +
                   szWhy + ' - and the setup log says more. An administrator takes it - takeown /F "' + g_aRestartFolders[index] + '" /A - restarts Windows, and runs setup ' +
                   'again. Nothing has been installed.';
         Exit;
      end;
      szDone := szDone + g_aRestartFolders[index] + ', ';
   end;
   Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: before this installation, ' + g_szRestartWhy + ', and a program that account started ' +
             'then may still hold it, or what is in it, open with that right, which no change of the permissions takes away. Setup has hardened ' + szDone + 'so that only ' +
             'SYSTEM and the Administrators may change them now - but for an entry an administrator gave a named account on a folder whose permissions no other account could ' +
             'change, which the setup log names - and does not install there until Windows has restarted, which closes every such handle: restart Windows, and run setup ' +
             'again. Nothing has been installed.';
end;

// Work item 78's R1 to R4, the first half, which changes nothing: the program folder,
// Bin and ControlPanel held from the root of their volume down (HoldFolderChain) until
// DeinitializeSetup, a link on the way gone through only when the administrators alone
// control it, and refused otherwise (R3); refused too, a program folder on a share or
// a network drive, one whose walks do not agree (a folder or a link made while setup
// looked), one on a volume that keeps no permissions or whose volume cannot be told -
// each walk's own, since a link may lead to another volume - one that is the root of a
// volume, or whose Bin or ControlPanel leads to one, one of the three owned by
// another account unless it is the earlier installation's (OwnerIsAllowed), and a
// program, a library, a folder or a link in Bin or ControlPanel that another account
// controls (CheckWhatIsInside, R4); and whether Bin and ControlPanel hold nothing, so
// that PrepareProgramFolders makes them again. Run from NextButtonClick on the Ready page, before
// the service is stopped, so that a refusal leaves a running server running; and from
// PrepareToInstall when it was not, or {app} has changed since. Earlier is the folder
// of the installation found before this one (PreviousInstallationFolder). '' when the
// installation may go on; otherwise the refusal, which PrepareToInstall gives (exit
// code 7 when silent).
function CheckProgramFolders(Earlier: String): String;
var
   szApp: String;
   szWhy: String;
   found: Integer;
begin
   Result := '';
   // Back from the page that follows, and on again: what an earlier pass made is
   // removed, while what it held is still held, and then let go.
   RemoveFoldersMadeHere();
   ReleaseHeldFolders();
   g_szFolderFindings := '';
   g_bRemakeBin := false;
   g_bRemakeControlPanel := false;
   g_szProgramFolderWaits := '';
   g_bFoldersChanged := false;
   SetArrayLength(g_aRestartFolders, 0);
   SetArrayLength(g_aRestartHandles, 0);
   SetArrayLength(g_aRestartData, 0);
   SetArrayLength(g_aRestartDistrust, 0);
   SetArrayLength(g_aStaleItems, 0);
   SetArrayLength(g_aStaleWhat, 0);
   g_szRestartWhy := '';
   // The handles of an earlier pass are let go above: none is read again.
   g_hProgramFolder := -1;
   g_hBin := -1;
   g_hControlPanel := -1;

   szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
   g_szFoldersCheckedFor := szApp;
   g_bFoldersChecked := true;
   g_bEarlierInstallation := false;
   if Trim(Earlier) <> '' then
      g_bEarlierInstallation := CompareText(RemoveBackslashUnlessRoot(Trim(Earlier)), szApp) = 0;
   g_szFolderAboveChecked := '';
   g_hProgramFolderPrepared := -1;
   g_hBinPrepared := -1;
   g_hControlPanelPrepared := -1;
   g_bProgramFolderHardened := false;
   CheckProgramFolderPermissions(szApp, false);

   // Work item 78's R2 and R3: not again before Windows has restarted, once setup has
   // hardened a folder another account could change, or refused for what a restart
   // must follow (RecordRestartNeeded).
   if RestartNeededFor(szApp, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: since Windows last started, setup has recorded (' + szWhy + ', in HKLM\' +
                HM_RESTART_KEY + ', which Windows does not keep past a restart) that a program another account started may hold a folder or a file there open with the ' +
                'right to change it, which only a restart takes away. Restart Windows, and run setup again. Nothing has been changed.';
      Exit;
   end;

   if not HoldFolderChain(szApp, 0, g_szProgramFolderReal, g_szProgramFolderHeld, g_hProgramFolder, g_bProgramFolderThere, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
      Exit;
   end;
   if not VolumeKeepsPermissions(g_hProgramFolder, g_szProgramFolderHeld, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Install hMailServer into a folder on an NTFS or ReFS volume, such as the default ' +
                ExpandConstant('{commonpf}') + '\hMailServer. Nothing has been changed.';
      Exit;
   end;
   // The root of a volume - where a mount point, or a junction to a volume's root,
   // leads (Inno Setup refuses a drive's root itself) - is not a program folder: its
   // permissions are the volume's, and hand down to what is made there, the uninstaller
   // and its record among it, which setup cannot make the administrators' alone.
   if IsVolumeRoot(g_szProgramFolderReal) then
   begin
      Result := 'Setup does not install into ' + szApp + ': it is the root of a volume (' + g_szProgramFolderReal + '), whose permissions are the volume''s and are handed ' +
                'down to what is made there, so that setup cannot keep the programs there the administrators'' alone. Install hMailServer into a folder on that volume, such as ' +
                AddBackslash(szApp) + 'hMailServer, or into the default ' + ExpandConstant('{commonpf}') + '\hMailServer. Nothing has been changed.';
      Exit;
   end;

   // Bin and ControlPanel, each by the same rules: a link there is on the way to what
   // setup copies, and may lead to another volume, whose folders above it are held to
   // the rule in their turn.
   g_szFolderAboveChecked := g_szProgramFolderReal;
   if not HoldFolderChain(AddBackslash(szApp) + 'Bin', 0, g_szBinReal, g_szBinHeld, g_hBin, g_bBinThere, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
      Exit;
   end;
   if not VolumeKeepsPermissions(g_hBin, g_szBinHeld, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Install hMailServer into a folder on an NTFS or ReFS volume, such as the default ' +
                ExpandConstant('{commonpf}') + '\hMailServer. Nothing has been changed.';
      Exit;
   end;
   if not HoldFolderChain(AddBackslash(szApp) + 'ControlPanel', 0, g_szControlPanelReal, g_szControlPanelHeld, g_hControlPanel, g_bControlPanelThere, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
      Exit;
   end;
   if not VolumeKeepsPermissions(g_hControlPanel, g_szControlPanelHeld, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Install hMailServer into a folder on an NTFS or ReFS volume, such as the default ' +
                ExpandConstant('{commonpf}') + '\hMailServer. Nothing has been changed.';
      Exit;
   end;
   if (not WalksAgree(g_szBinHeld, g_bBinThere)) or (not WalksAgree(g_szControlPanelHeld, g_bControlPanelThere)) then
   begin
      Result := 'Setup does not install into ' + szApp + ': a folder on the way to it, to Bin or to ControlPanel changed while setup was looking at it - a folder or a link made, ' +
                'or one removed - so setup does not install there; run setup again to have it looked at. Nothing has been changed.';
      Exit;
   end;
   // Work item 78's R2 and R3 (check 5 of R1 to R4, 27 October 2026): the record of a
   // restart looked up again by where the walks ended, so that another name for a folder
   // setup hardened, or refused, does not find setup willing to install before the restart.
   if RestartRecordedForWalked(szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ' until Windows has restarted: since Windows last started, setup has recorded (' + szWhy + ', in HKLM\' +
                HM_RESTART_KEY + ', which Windows does not keep past a restart) that a program another account started may hold a folder or a file of the same folder, ' +
                'reached by another name, open with the right to change it, which only a restart takes away. Restart Windows, and run setup again. Nothing has been changed.';
      Exit;
   end;
   if IsVolumeRoot(g_szBinReal) or IsVolumeRoot(g_szControlPanelReal) then
   begin
      Result := 'Setup does not install into ' + szApp + ': its Bin or its ControlPanel is a link to the root of a volume, whose permissions are the volume''s. An administrator ' +
                'removes the link - rmdir "' + AddBackslash(szApp) + 'Bin" or rmdir "' + AddBackslash(szApp) + 'ControlPanel", which leaves the volume as it is - and runs setup ' +
                'again, which makes the folder itself. Nothing has been changed.';
      Exit;
   end;

   // Work item 78's R2: Bin and ControlPanel that were there and hold nothing are made
   // again by PrepareProgramFolders, not hardened - decided now, before anything
   // changes: the program folder's hardening hands its permissions down to them, after
   // which who could write one before can no longer be read from it (RemakeEmptyFolder).
   g_bRemakeBin := g_bBinThere and FolderHoldsNothing(g_szBinReal);
   g_bRemakeControlPanel := g_bControlPanelThere and FolderHoldsNothing(g_szControlPanelReal);
   // And whether the program folder waits for the files, decided now too, once: the
   // restart rule counts a right to write it by this answer (NoteFoldersOthersCouldChange),
   // and PrepareProgramFolders leaves it unhardened by this answer alone (check 7 of R1 to
   // R4, 28 October 2026: asked again there, after the service stop, a data folder renamed
   // in the meantime decided it).
   if g_bProgramFolderThere then
      g_szProgramFolderWaits := ProgramFolderWaitsForTheFiles();

   // The owners of the three, and what is in Bin and ControlPanel.
   if g_bProgramFolderThere then
      if not OwnerIsAllowed(g_szProgramFolderReal, g_hProgramFolder, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
         Exit;
      end;
   if g_bBinThere then
      if not OwnerIsAllowed(g_szBinReal, g_hBin, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
         Exit;
      end;
   if g_bControlPanelThere then
      if not OwnerIsAllowed(g_szControlPanelReal, g_hControlPanel, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been changed.';
         Exit;
      end;

   found := 0;
   szWhy := '';
   if g_bBinThere then
      CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBin), 0, false, false, found, szWhy);
   if g_bControlPanelThere then
      CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanel), 0, false, false, found, szWhy);
   if g_bProgramFolderThere then
      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, false, found, szWhy);
   if found > 0 then
   begin
      Result := 'Setup does not install into ' + szApp + ', since the service and the administrators would run programs there that another account controls: ' + szWhy + 'Nothing has been changed.';
      Exit;
   end;

   // Work item 78's R2 and R3: what another account could change before this
   // installation, which HardenForRestart hardens before it refuses.
   NoteFoldersOthersCouldChange();
end;

// Work item 78's R1 to R4 before the service is stopped (NextButtonClick on the Ready
// page) or, when that check was not of this {app}, in PrepareToInstall
// (PrepareProgramFolders): the folders looked at, changing nothing (CheckProgramFolders),
// and then, when nothing was refused, what another account could change hardened and
// the installation refused until Windows has restarted (HardenForRestart). '' when the
// installation may go on; otherwise the refusal.
function CheckFoldersBeforeTheStop(Earlier: String): String;
begin
   Result := CheckProgramFolders(Earlier);
   if Result = '' then
      Result := HardenForRestart();
end;

// The refusal of a folder PrepareProgramFolders holds - the program folder, Bin or
// ControlPanel - that HeldIsLink, read through the handle that holds it, finds a link
// now, or cannot read: an empty folder can be made one through a handle a program
// opened on it while another account could write it, whatever its permissions say
// since, and what goes by its path then goes where the link leads. A folder found
// changed (g_bFoldersChanged), for PrepareToInstall.
function PreparedFolderIsLink(Folder: String): String;
begin
   g_bFoldersChanged := true;
   Result := Folder + ' is a junction, a symbolic link or a mount point now, or what it is could not be read: it was made one while setup was preparing the installation, so ' +
             'setup does not install there. An administrator removes the link - rmdir "' + Folder + '", which leaves what it leads to as it is - restarts Windows, so that ' +
             'nothing opened while another account could change the folder stays open, and runs setup again. Nothing has been installed.';
end;

// Work item 78's R1 to R4, the second half, in PrepareToInstall: before anything is
// installed, and before setup runs any program of the installation, the program
// folder made, or hardened, Bin and ControlPanel with it, so that none of the three
// lets anybody but SYSTEM and the Administrators change it - but for an entry of its
// own that gives a named account a right to write, which setup says
// (SayWriteEntriesKept). What CheckProgramFolders found and holds is what is made or
// hardened: nothing is looked at again by its path. '' when the installation may go
// on; otherwise the refusal - PrepareToInstall's answer, exit code 7 when silent.
//   1. Not there: made by setup with its permissions from the start, and the folders
//      above it that are not there either, below the deepest one the walk held
//      (MakeFolders, R1), and Bin in it, and ControlPanel when it is installed, made
//      the same way, so that what setup made is all recorded and removed again after
//      a cancel (RemoveFoldersMadeHere). What Inno Setup then makes in them inherits
//      those.
//   2. There: the program folder hardened (HardenHeldFolder, R2) - unless it is the
//      earlier installation's and holds a server installation whose Data, Logs, Temp,
//      Database or Events still inherit from it (an installation before 6.3.6, whose
//      /ProtectFolders made them private): its change would then be handed down
//      through the message store, so hMailServer.exe /ProtectFolders makes those
//      private and hardens it after the files, as it does at every installation - and
//      no other account could change it, or CheckFoldersBeforeTheStop would have
//      hardened it and asked for a restart. The root of a volume CheckProgramFolders has
//      refused. A folder hardened in place is given a file setup keeps in it until the
//      files are in (MakePlaceholder), so that it cannot be made a junction whatever a
//      handle opened on it before may do.
//   3. Then Bin and ControlPanel: one that held nothing when CheckProgramFolders looked
//      removed and made again (RemakeEmptyFolder), one that holds something refused if
//      it is a link now and otherwise hardened before its files and given the file
//      setup keeps, one that is not there made by setup with its permissions
//      (ControlPanel when it is installed); the three read again through their handles,
//      and refused if any is a link now; and what is in them looked at again, now that
//      only the administrators can add to them (CheckWhatIsInside, R4).
// Any of these that cannot be done is a refusal: nothing is installed into a folder
// setup could not make or harden. One that finds a folder changed since setup looked -
// a link, a name taken, something put in it - sets g_bFoldersChanged, and
// PrepareToInstall then sets the service not to start. A folder another account could
// change is never hardened and installed into in one run: CheckFoldersBeforeTheStop has
// refused that until Windows has restarted, and so an owner other than the three is not
// taken here (Adopt false). The setup log says what was found and done; an attended
// installation shows what an administrator is to look at (g_szFolderFindings) in one
// box.
function PrepareProgramFolders(Earlier: String): String;
var
   szApp: String;
   szWhy: String;
   szInherits: String;
   index: Integer;
   found: Integer;
   hMade: Integer;
   hInside: Integer;
   bKnown: Boolean;
begin
   Result := '';
   szApp := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
   if (not g_bFoldersChecked) or (CompareText(g_szFoldersCheckedFor, szApp) <> 0) then
      g_szFolderRefusal := CheckFoldersBeforeTheStop(Earlier);
   // Back to the folder page and on again: looked at again.
   g_bFoldersChecked := false;
   if g_szFolderRefusal <> '' then
   begin
      Result := g_szFolderRefusal;
      Exit;
   end;

   if not g_bProgramFolderThere then
   begin
      if not MakeFolders(g_szProgramFolderHeld, g_hProgramFolder, g_szProgramFolderReal, 0, hMade, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
         Exit;
      end;
      g_hProgramFolderPrepared := hMade;
      g_bProgramFolderHardened := true;
      if MadeParentOffset() > 0 then
         for index := 0 to MadeParentOffset() - 1 do
            Log(g_aFoldersMade[index] + ', which setup made, is not recorded for the uninstaller, which removes the eight nearest the program folder; it stays when hMailServer is uninstalled.');
      // Bin, and ControlPanel when it is installed, made in it now, recorded as the
      // program folder is (check 4 of R1 to R4, 27 October 2026: a Bin MoveIni made at
      // ssInstall was recorded nowhere, and a cancel then left it, and the program
      // folder and every folder above it that setup had made, behind).
      if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'Bin', 1, hInside, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
         Exit;
      end;
      g_hBinPrepared := hInside;
      if ControlPanelIsInstalled() then
      begin
         if not MakeFolders(g_szProgramFolderReal, hMade, AddBackslash(g_szProgramFolderReal) + 'ControlPanel', 2, hInside, szWhy) then
         begin
            Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
            Exit;
         end;
         g_hControlPanelPrepared := hInside;
      end;
      Exit;
   end;

   // Whether the program folder waits for the files is CheckProgramFolders' answer,
   // given before anything changed, which the restart rule went by; asked again now,
   // after the service stop, a different answer is a data folder made, renamed or
   // removed in the meantime - an account that may write the program folder can rename
   // an empty folder in it to Data - and is refused, not taken (check 7 of R1 to R4, 28
   // October 2026).
   szInherits := g_szProgramFolderWaits;
   if CompareText(ProgramFolderWaitsForTheFiles(), szInherits) <> 0 then
   begin
      g_bFoldersChanged := true;
      Result := 'Setup does not install into ' + szApp + ': a data folder of the earlier installation in ' + g_szProgramFolderReal + ' - Data, Logs, Temp, Database or Events - ' +
                'was made, renamed or removed, or its permissions changed, while setup was preparing the installation, so setup does not install there. An administrator ' +
                'looks at the program folder, restarts Windows, so that nothing opened while another account could change it stays open, and runs setup again. Nothing has ' +
                'been installed.';
      Exit;
   end;
   if szInherits <> '' then
   begin
      Log('The program folder ' + g_szProgramFolderReal + ' is not hardened before the files: it holds the earlier installation''s server, whose ' + szInherits + ' still inherits from it, and the change ' +
          'would be handed down through the message store. No inherited entry and no entry of a group of every user lets another account change it, or change what is in it ' +
          '(CheckFoldersBeforeTheStop would have hardened it and refused until Windows has restarted), and an entry of its own for a named account is said below. ' +
          'hMailServer.exe /ProtectFolders makes the data folders private and then hardens it after the files' +
          ' (an installation of the Control Panel alone runs none: an administrator runs "hMailServer.exe /ProtectFolders" from its Bin folder). Bin and ControlPanel are hardened, or made, before the files.');
      SayWriteEntriesKept(g_szProgramFolderReal, g_hProgramFolder);
   end
   else if not HardenHeldFolder(g_szProgramFolderReal, g_hProgramFolder, false, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + g_szProgramFolderReal + ' could not be hardened before the files - ' + szWhy + ' - and the setup log says more. ' +
                'An administrator takes it - takeown /F "' + g_szProgramFolderReal + '" /A - and runs setup again, or installs into a folder setup makes. Nothing has been installed.';
      Exit;
   end
   else if not MakePlaceholder(g_szProgramFolderReal, g_hProgramFolder, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. An administrator looks at it, restarts Windows, and runs setup again. Nothing has been installed.';
      Exit;
   end
   else
      g_bProgramFolderHardened := true;
   g_hProgramFolderPrepared := g_hProgramFolder;

   // Bin and ControlPanel: one that held nothing when CheckProgramFolders looked made
   // again (RemakeEmptyFolder), one that holds something refused if it was made a link
   // since the walk and hardened otherwise, one that is not there made.
   if g_bBinThere then
   begin
      if g_bRemakeBin then
      begin
         if not RemakeEmptyFolder(g_szBinReal, g_hBin, 1, hMade, szWhy) then
         begin
            Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
            Exit;
         end;
         g_hBinPrepared := hMade;
      end
      else if HeldIsLink(g_hBin, bKnown) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal);
         Exit;
      end
      else if not HardenHeldFolder(g_szBinReal, g_hBin, false, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + g_szBinReal + ' could not be hardened before the files - ' + szWhy + ' - and the setup log says more. ' +
                   'An administrator takes it - takeown /F "' + g_szBinReal + '" /A - and runs setup again, or installs into a folder setup makes. Nothing has been installed.';
         Exit;
      end
      else if not MakePlaceholder(g_szBinReal, g_hBin, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. An administrator looks at it, restarts Windows, and runs setup again. Nothing has been installed.';
         Exit;
      end
      else
         g_hBinPrepared := g_hBin;
   end
   else if not MakeFolders(g_szBinHeld, g_hBin, g_szBinReal, 1, hMade, szWhy) then
   begin
      Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
      Exit;
   end
   else
      g_hBinPrepared := hMade;

   if g_bControlPanelThere then
   begin
      if g_bRemakeControlPanel then
      begin
         if not RemakeEmptyFolder(g_szControlPanelReal, g_hControlPanel, 2, hMade, szWhy) then
         begin
            Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
            Exit;
         end;
         g_hControlPanelPrepared := hMade;
      end
      else if HeldIsLink(g_hControlPanel, bKnown) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal);
         Exit;
      end
      else if not HardenHeldFolder(g_szControlPanelReal, g_hControlPanel, false, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + g_szControlPanelReal + ' could not be hardened before the files - ' + szWhy + ' - and the setup log says more. ' +
                   'An administrator takes it - takeown /F "' + g_szControlPanelReal + '" /A - and runs setup again, or installs into a folder setup makes. Nothing has been installed.';
         Exit;
      end
      else if not MakePlaceholder(g_szControlPanelReal, g_hControlPanel, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. An administrator looks at it, restarts Windows, and runs setup again. Nothing has been installed.';
         Exit;
      end
      else
         g_hControlPanelPrepared := g_hControlPanel;
   end
   else if ControlPanelIsInstalled() then
   begin
      if not MakeFolders(g_szControlPanelHeld, g_hControlPanel, g_szControlPanelReal, 2, hMade, szWhy) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + szWhy + '. Nothing has been installed.';
         Exit;
      end;
      g_hControlPanelPrepared := hMade;
   end;

   // The three read again through the handles that hold them, just before the last
   // look, which goes by their paths: one made a link since - an empty folder, through
   // a handle opened on it while another account could write it - is refused.
   if g_hProgramFolderPrepared <> -1 then
      if HeldIsLink(g_hProgramFolderPrepared, bKnown) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szProgramFolderReal);
         Exit;
      end;
   if g_hBinPrepared <> -1 then
      if HeldIsLink(g_hBinPrepared, bKnown) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szBinReal);
         Exit;
      end;
   if g_hControlPanelPrepared <> -1 then
      if HeldIsLink(g_hControlPanelPrepared, bKnown) then
      begin
         Result := 'Setup does not install into ' + szApp + ': ' + PreparedFolderIsLink(g_szControlPanelReal);
         Exit;
      end;

   // What is in Bin and ControlPanel, and in the program folder's other folders, now
   // that only the administrators can add to it: what is refused now was not there, or
   // not so, when CheckProgramFolders looked.
   found := 0;
   szWhy := '';
   if g_bBinThere then
      CheckWhatIsInside(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, true, false, found, szWhy);
   if g_bControlPanelThere then
      CheckWhatIsInside(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, true, false, found, szWhy);
   if g_bProgramFolderThere then
      CheckWhatElseIsInside(g_szProgramFolderReal, g_hProgramFolder, true, found, szWhy);
   if found > 0 then
   begin
      g_bFoldersChanged := true;
      Result := 'Setup does not install into ' + szApp + ', since the service and the administrators would run programs there that another account controls: ' + szWhy + 'Nothing has been installed.';
      Exit;
   end;

   if (g_szFolderFindings <> '') and (not WizardSilent()) then
      SuppressibleMsgBox('Setup found this in the program folder ' + szApp + ', and the installation goes on:' + #13#10#13#10 + g_szFolderFindings + 'The setup log says the same.',
                         mbInformation, MB_OK, IDOK);
end;

// Work item 78's R2 and R3, after the files: a link, or an empty folder, at any depth
// in Folder - Bin, ControlPanel, or one of the folders that hold what is run - which
// only something done while setup installed can have left there, since setup removed
// the empty folders that were there before (CheckWhatIsInside) and Inno Setup makes no
// empty one: an empty folder made a junction through a handle opened before setup
// hardened it, whose files went where it led, or one the junction was taken off again.
// Each is named in Why and counted in Found, and nothing is removed: the installation
// fails, and an administrator looks (check 4 of R1 to R4, 27 October 2026 - setup
// removed each by its path, and through a folder made a junction it removed links and
// empty folders wherever that led). Each folder is opened as itself and held while it
// is looked at, a link not followed, and refused, not walked, unless it is no link and
// the folder its path names - Expected, read before through the handle of the folder
// above it, or the prepared folder's (FoldersStillAsPrepared). Nothing when Folder is
// not there at the first level.
function NothingTurnedIn(Folder: String; Expected: String; Depth: Integer; var Found: Integer; var Why: String): Boolean;
var
   rec: TFindRec;
   folders: array of String;
   count: Integer;
   index: Integer;
   folderHandle: Integer;
   szItem: String;
   szWhat: String;
   szFinal: String;
   bFolder: Boolean;
   bLink: Boolean;
   bKnown: Boolean;
begin
   Result := true;
   count := 0;
   if Depth > 32 then
      Exit;
   if (Depth = 0) and (FolderAttributes(Folder) = HM_INVALID_ATTRIBUTES) then
      Exit;
   folderHandle := HoldFolder(Folder, HM_HOLD_FOLDER_ACCESS, HM_SHARE_ALL_BUT_DELETE, 0, HM_OPEN_EXISTING, HM_HOLD_FOLDER_FLAGS, 0);
   szFinal := '';
   if folderHandle <> -1 then
      if not HeldIsLink(folderHandle, bKnown) then
         szFinal := FinalPathOfHeld(folderHandle);
   if (szFinal = '') or (CompareText(szFinal, Expected) <> 0) then
   begin
      if folderHandle <> -1 then
         ReleaseFolder(folderHandle);
      Found := Found + 1;
      Why := Why + Folder + ' is a link, which setup did not leave, or not the folder setup prepared there, or could not be opened as itself. ';
      Log('The program folder after the files: ' + Folder + ' is a link, which setup did not leave, or not the folder setup prepared there, or could not be opened as itself: ' +
          'setup does not look through it, and removes nothing.');
      Result := false;
      Exit;
   end;
   if FindFirst(AddBackslash(Folder) + '*', rec) then
   begin
      try
         repeat
            if (rec.Name <> '.') and (rec.Name <> '..') then
            begin
               szItem := AddBackslash(Folder) + rec.Name;
               bFolder := (rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0;
               bLink := (rec.Attributes and HM_REPARSE_POINT) <> 0;
               if bLink or (bFolder and FolderHoldsNothing(szItem)) then
               begin
                  if bLink then
                     szWhat := 'is a link'
                  else
                     szWhat := 'is an empty folder';
                  Found := Found + 1;
                  Why := Why + szItem + ' ' + szWhat + ', which setup did not leave. ';
                  Log('The program folder after the files: ' + szItem + ' ' + szWhat + ', which setup did not leave, and setup leaves it as it is for an administrator to look at.');
                  Result := false;
               end
               else if bFolder then
               begin
                  SetArrayLength(folders, count + 1);
                  folders[count] := szItem;
                  count := count + 1;
               end;
            end;
         until not FindNext(rec);
      finally
         FindClose(rec);
      end;
   end;
   for index := 0 to count - 1 do
      if not NothingTurnedIn(folders[index], AddBackslash(szFinal) + ExtractFileName(folders[index]), Depth + 1, Found, Why) then
         Result := false;
   ReleaseFolder(folderHandle);
end;

// One folder PrepareProgramFolders made or hardened, read again after the files through
// the handle that holds it: no link now (HeldIsLink), Item - a file setup installs there,
// or Bin in the program folder - reached by its path in it (ItemIsIn), and its
// permissions as setup left them when Hardened (HeldFolderIsHardened) - with no entry
// for any account but the three when AnyAccount is set, for a folder setup made with
// permissions of its own, which have none. Why says what is not.
function PreparedFolderIsAsLeft(Folder: String; Handle: Integer; Item: String; Hardened: Boolean; AnyAccount: Boolean; var Why: String): Boolean;
var
   bKnown: Boolean;
   szWhy: String;
begin
   Result := false;
   if HeldIsLink(Handle, bKnown) then
      Why := Why + Folder + ', which setup prepared, is a junction, a symbolic link or a mount point now, or could not be read. '
   else if (Item <> '') and (not ItemIsIn(Item, Handle)) then
      Why := Why + Item + ' is not in ' + Folder + ', the folder setup prepared: the path leads elsewhere now. '
   else if Hardened and (not HeldFolderIsHardened(Handle, AnyAccount, false, szWhy)) then
      Why := Why + Folder + ' is no longer as setup made or hardened it: ' + szWhy + '. '
   else
      Result := true;
end;

// Work item 78's R2 and R3: before setup runs any program of the installation
// (CurStepChanged, ssPostInstall), what PrepareProgramFolders made or hardened read again
// through the handles that hold it - a program that opened one of the folders while
// another account could change it keeps what it opened it for, which setup's hardening
// and its own reads cannot see - and what is in Bin, ControlPanel and the folders that
// hold what is run looked at for a link or an empty folder setup did not leave
// (NothingTurnedIn): Bin and ControlPanel only when each was found as setup left it,
// from where its handle says it is, and Addons, Events, DBScripts, WebAdmin and
// Languages only when the program folder was, each only as the folder of its name in
// it - never through a link, whatever was found (check 4 of R1 to R4, 27 October 2026).
// True when all is as setup left it; Why says what is not.
function FoldersStillAsPrepared(var Why: String): Boolean;
var
   szApp: String;
   szAppFinal: String;
   szFolder: String;
   index: Integer;
   found: Integer;
begin
   Why := '';
   found := 0;
   szApp := AddBackslash(ExpandConstant('{app}'));
   szAppFinal := '';
   if g_hProgramFolderPrepared <> -1 then
      if PreparedFolderIsAsLeft(g_szProgramFolderReal, g_hProgramFolderPrepared, szApp + 'Bin', g_bProgramFolderHardened, g_bProgramFolderNew, Why) then
         szAppFinal := FinalPathOfHeld(g_hProgramFolderPrepared);
   if g_hBinPrepared <> -1 then
      if PreparedFolderIsAsLeft(g_szBinReal, g_hBinPrepared, szApp + 'Bin\hMailServer.INI', true, SetupMadeBin(), Why) then
         NothingTurnedIn(g_szBinReal, FinalPathOfHeld(g_hBinPrepared), 0, found, Why);
   if g_hControlPanelPrepared <> -1 then
   begin
      if ControlPanelIsInstalled() then
      begin
         if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, szApp + 'ControlPanel\hMailCP.exe', true, SetupMadeControlPanel(), Why) then
            NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why);
      end
      else if PreparedFolderIsAsLeft(g_szControlPanelReal, g_hControlPanelPrepared, '', true, SetupMadeControlPanel(), Why) then
         NothingTurnedIn(g_szControlPanelReal, FinalPathOfHeld(g_hControlPanelPrepared), 0, found, Why);
   end;
   if szAppFinal <> '' then
      for index := 0 to 4 do
      begin
         case index of
            0: szFolder := 'Addons';
            1: szFolder := 'Events';
            2: szFolder := 'DBScripts';
            3: szFolder := 'WebAdmin';
            4: szFolder := 'Languages';
         end;
         NothingTurnedIn(AddBackslash(g_szProgramFolderReal) + szFolder, AddBackslash(szAppFinal) + szFolder, 0, found, Why);
      end;
   Result := Why = '';
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


// A connection to this computer's Service Control Manager, for opening a service by
// its name (OpenSCManagerW with SC_MANAGER_CONNECT); 0 when Windows refuses it, which
// is said, in the log and to whoever runs setup or the uninstaller (each named as
// what it is, since the uninstaller runs this code too), at each refusal, since
// whatever asked then cannot tell whether the hMailServer service is there, running
// or stopped.
function OpenServiceManager() : SC_HANDLE;
var
   lastError: Integer;
   who: String;
begin
   Result := OpenSCManagerW(0, 0, SC_MANAGER_CONNECT);
   if (Result = 0) then
   begin
      lastError := DLLGetLastError();
      if IsUninstaller() then
         who := 'The uninstaller'
      else
         who := 'Setup';
      Log('Windows did not open its Service Control Manager to ' + Lowercase(who) + ' (error ' + IntToStr(lastError) + '): ' + SysErrorMessage(lastError));
      SuppressibleMsgBox(who + ' could not open the Service Control Manager of this computer, so it cannot tell whether the hMailServer service is installed or running, or stop it.' + #13#10#13#10 +
             'Windows said: ' + SysErrorMessage(lastError), mbError, MB_OK, IDOK);
   end;
end;

// The service called Name (its service name, not its display name), opened with
// Access through a connection made for it, which Manager is given; 0, with Manager
// closed and 0, when the connection or the service could not be opened - the
// service is not installed, or Windows does not grant that access to it. The
// caller lets both go with CloseServiceAndManager.
function OpenNamedService(Name: String; Access: cardinal; var Manager: SC_HANDLE) : SC_HANDLE;
begin
   Result := 0;
   Manager := OpenServiceManager();
   if (Manager = 0) then
      Exit;
   Result := OpenServiceW(Manager, Name, Access);
   if (Result = 0) then
   begin
      CloseServiceHandle(Manager);
      Manager := 0;
   end;
end;

procedure CloseServiceAndManager(Service: SC_HANDLE; Manager: SC_HANDLE);
begin
   if (Service <> 0) then
      CloseServiceHandle(Service);
   if (Manager <> 0) then
      CloseServiceHandle(Manager);
end;

// The state the service called Name is in, as QueryServiceStatusEx reports it
// (SERVICE_STOPPED, SERVICE_RUNNING or one of the pending and paused states); 0,
// which is none of them, when it is not installed or its state cannot be read.
function GetServiceState(Name: String) : cardinal;
var
   manager: SC_HANDLE;
   service: SC_HANDLE;
   status : SERVICE_STATUS_PROCESS;
   needed : cardinal;
begin
   Result := 0;
   service := OpenNamedService(Name, SERVICE_QUERY_STATUS, manager);
   if (service = 0) then
      Exit;
   if QueryServiceStatusProcess(service, SERVICE_STATUS_PROCESS_INFO, status, SERVICE_STATUS_PROCESS_SIZE, needed) then
      Result := status.dwCurrentState;
   CloseServiceAndManager(service, manager);
end;

// Whether Windows knows a service called Name: True when it can be opened to read
// its configuration.
function IsServiceInstalled(Name: String) : Boolean;
var
   manager: SC_HANDLE;
   service: SC_HANDLE;
begin
   service := OpenNamedService(Name, SERVICE_QUERY_CONFIG, manager);
   Result := (service <> 0);
   CloseServiceAndManager(service, manager);
end;

// Asks the service called Name to stop (SERVICE_CONTROL_STOP) and returns at once:
// True when Windows passed the request on, False when the service could not be
// opened to stop it or Windows refused the request - a service that is stopped
// already is refused (ERROR_SERVICE_NOT_ACTIVE). Whoever asks waits for the stop
// (StopHMailServerAndWaitForExit, the one caller).
function StopService(Name: String) : Boolean;
var
   manager: SC_HANDLE;
   service: SC_HANDLE;
   status : SERVICE_STATUS;
   lastError  : Integer;
begin
   Result := false;
   service := OpenNamedService(Name, SERVICE_STOP, manager);
   if (service = 0) then
      Exit;
   Result := ControlService(service, SERVICE_CONTROL_STOP, status);
   if not Result then
   begin
      lastError := DLLGetLastError();
      Log('Windows did not pass the stop on to the ' + Name + ' service (error ' + IntToStr(lastError) + '): ' + SysErrorMessage(lastError));
   end;
   CloseServiceAndManager(service, manager);
end;

// Whether the service called Name is running, and whether it is stopped. A
// service that is starting, stopping or paused is neither, and so is one that is
// not installed or whose state cannot be read.
function IsServiceRunning(Name: String) : Boolean;
begin
   Result := (GetServiceState(Name) = SERVICE_RUNNING);
end;

function IsServiceStopped(Name: String) : Boolean;
begin
   Result := (GetServiceState(Name) = SERVICE_STOPPED);
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
   manager: SC_HANDLE;
   service: SC_HANDLE;
   status : SERVICE_STATUS_PROCESS;
   needed : cardinal;
begin
   Result := 0;
   service := OpenNamedService('hMailServer', SERVICE_QUERY_STATUS, manager);
   if (service = 0) then
      Exit;
   if QueryServiceStatusProcess(service, SERVICE_STATUS_PROCESS_INFO, status, SERVICE_STATUS_PROCESS_SIZE, needed) then
      Result := status.dwProcessId;
   CloseServiceAndManager(service, manager);
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

// The settings file of the installation found before this one: the one in its
// folder, Folder (PreviousInstallationFolder), or, for a very old installation, the
// one in the Windows folder. '' when there is none.
function PreviousSettingsFile(Folder: String): String;
begin
   Result := '';
   if (Folder <> '') and FileExists(AddBackslash(Folder) + 'Bin\hMailServer.ini') then
      Result := AddBackslash(Folder) + 'Bin\hMailServer.ini'
   else if FileExists(ExpandConstant('{win}\hMailServer.ini')) then
      Result := ExpandConstant('{win}\hMailServer.ini');
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
   szFolder := PreviousInstallationFolder();
   szFile := PreviousSettingsFile(szFolder);

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
// or changed. Nothing is asked of an installation whose hMailServer.exe is 6 or
// later - every one of this fork's - or is missing or has no version, and a stopped
// service is not started for it: COM activation would start it.
function OriginalHMailServerRefusal(): String;
var
   szFolder: String;
   szExe: String;
   szServerVersion: String;
   wMajor, wMinor, wRevision, wBuild: Word;
   iMajor, iMinor: Integer;
   iDatabase: Integer;
   bScripts: Boolean;
   bForkSecret: Boolean;
   szSettings: String;
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
   if not IsServerBeforeThisFork(iMajor, iMinor) then
      Exit;

   Log('The installation in ' + szFolder + ' has hMailServer.exe ' + szServerVersion + ', which is not this fork''s 6.x and may be the original hMailServer''s 5.7: its database''s version decides.');
   iDatabase := -1;
   if IsServiceRunning('hMailServer') then
      iDatabase := ReadDatabaseVersion('before this installation, to tell the original''s 5.7 from a database this build upgrades')
   else
      Log('The hMailServer service is not running, so the database''s version is not read; the settings file and the DBScripts folder decide.');

   bScripts := False;
   bForkSecret := False;
   if iDatabase <= 0 then
   begin
      // The settings file the installed server reads. The original's 5.7.0 build
      // 3013 and later read hMailServer.ini in ProgramData before the one in Bin,
      // and keep a new installation's there; this fork never writes that file, so
      // a stale Bin file of this fork's beside it is not the one such a server
      // reads. Its earlier builds - 2788 and 2843, its releases up to 5.6 and its
      // 5.7 development builds up to 5708 - read the one in Bin alone
      // (IsBeforeProgramDataSettings), and a ProgramData file the uninstall of a
      // later 5.7 left behind is not theirs. Its CI builds 5.7.0.2948, 2951 and
      // 2957, of the branch that brought that layout, and 5.7.0.2958 to 3012 read
      // the ProgramData file first as well, but are taken here as its earlier ones.
      szSettings := '';
      if not IsBeforeProgramDataSettings(iMajor, iMinor, wRevision, wBuild) then
         szSettings := ExpandConstant('{commonappdata}\hMailServer\hMailServer.ini');
      if (szSettings = '') or not FileExists(szSettings) then
         szSettings := PreviousSettingsFile(szFolder);
      if szSettings <> '' then
      begin
         Log('The installed server''s settings file is ' + szSettings + '.');
         bForkSecret := IsForkOnlyPasswordEncryption(GetIniInt('Database', 'PasswordEncryption', 0, 0, 0, szSettings));
      end
      else
         Log('No settings file of the installation in ' + szFolder + ' was found.');
      if bForkSecret then
         Log('The settings file ' + szSettings + ' keeps the database''s password as only this fork does ([Database] PasswordEncryption ' +
             IntToStr(FORK_PASSWORD_ENCRYPTION) + '), which the original cannot read: the database is this fork''s own, and the DBScripts folder is not asked.')
      else if FindFirst(AddBackslash(szFolder) + 'DBScripts\Upgrade' + IntToStr(ORIGINAL_LAST_SHARED_SCHEMA) + 'to*.sql', FindRec) then
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

   iKind := OriginalHMailServerKind(iMajor, iMinor, iDatabase, bScripts, bForkSecret);
   Result := OriginalHMailServerMessage(iKind, szServerVersion, iDatabase, szFolder);
   if Result = '' then
      Log('Not refused as the original''s 5.7: neither the database''s version nor, with that unread, the settings file or the DBScripts folder shows a database this build cannot upgrade.');
end;

// The file a refusal is written to as well as the log, so that a silent setup run
// without /LOG, whose message box is suppressed, still leaves the reason where it
// can be found: hMailServer-setup-refused.txt in the folder TEMP names in setup's
// environment - the temporary folder of the account setup runs as, elevated, or
// the one a package manager that started it set, which Chocolatey's install script
// reads back - or, with TEMP unset, Inno Setup's own temporary folder.
// InitializeSetup deletes one an earlier run left when this one is not refused.
function RefusalFileName(): String;
begin
   Result := GetEnv('TEMP');
   if Result = '' then
      Result := GetTempDir();
   Result := AddBackslash(Result) + 'hMailServer-setup-refused.txt';
end;

procedure WriteRefusalFile(szRefusal: String);
var
   szFile: String;
begin
   szFile := RefusalFileName();
   if WriteOriginalHMailServerRefusal(szFile, szRefusal) then
      Log('The refusal is also in ' + szFile + '.')
   else
      Log('The refusal could not be written to ' + szFile + '.');
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
	// refusal is in its log (with /LOG) and in the file RefusalFileName names.
	szRefusal := OriginalHMailServerRefusal();
	if szRefusal <> '' then
	begin
		Log('Refused: ' + szRefusal);
		WriteRefusalFile(szRefusal);
		SuppressibleMsgBox(szRefusal, mbCriticalError, MB_OK, IDOK);
		Result := false;
		Exit;
	end;
	if FileExists(RefusalFileName()) then
		DeleteFile(RefusalFileName());

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

// The line setup's log has when the folders it prepares were found changed - after
// the files (FoldersStillAsPrepared, exit code 12), or in PrepareProgramFolders after
// the service was stopped (g_bFoldersChanged, exit code 7): hMailServer.Updater reads it
// (FoldersFoundChanged) and puts no earlier version back over such folders, whose
// installer would install through them and start the service again (check 4 of R1 to
// R4, 27 October 2026).
function FoldersChangedLine(): String;
begin
   Result := 'Setup found the folders it prepares changed (work item 78): the hMailServer service is set not to start, and no earlier version is to be put back over them.';
end;

// When the folders setup prepared are not as it left them after the files
// (FoldersStillAsPrepared), or were changed while it prepared them (PrepareToInstall):
// an installed service set not to start, since its program is reached through them,
// until an administrator has looked and run setup again - which, its folders as it left
// them after the files, sets it to start again (RestoreServiceStart), as the mark it
// leaves in HKLM\SOFTWARE\hMailServer (SetupKeptServiceFromStarting) says it may.
procedure KeepServiceFromStarting();
var
   ResultCode: Integer;
begin
   if not IsServiceInstalled('hMailServer') then
      Exit;
   if Exec(ExpandConstant('{sys}\sc.exe'), 'config hMailServer start= disabled', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
   begin
      RegWriteDWordValue(HKLM, 'SOFTWARE\hMailServer', 'SetupKeptServiceFromStarting', 1);
      Log('The hMailServer service is set not to start (sc config hMailServer start= disabled) until an administrator has looked at what is said and run setup again.');
   end
   else
      Log('The hMailServer service could not be set not to start (sc config hMailServer start= disabled returned ' + IntToStr(ResultCode) + '); an administrator does so before it starts.');
end;

// Work item 78: a service setup set not to start (KeepServiceFromStarting), set to start
// again (sc config hMailServer start= auto, as /Register makes it) once a later
// installation has found the folders as it left them after the files, before its steps
// start the service - /Register does not change how a service starts, and a service
// left disabled would not start. Nothing when setup left no mark.
procedure RestoreServiceStart();
var
   ResultCode: Integer;
   mark: cardinal;
begin
   if not RegQueryDWordValue(HKLM, 'SOFTWARE\hMailServer', 'SetupKeptServiceFromStarting', mark) then
      Exit;
   if not IsServiceInstalled('hMailServer') then
      Exit;
   if Exec(ExpandConstant('{sys}\sc.exe'), 'config hMailServer start= auto', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
   begin
      RegDeleteValue(HKLM, 'SOFTWARE\hMailServer', 'SetupKeptServiceFromStarting');
      Log('The hMailServer service, which an earlier run of setup set not to start, is set to start again (sc config hMailServer start= auto): the folders are as setup left them.');
   end
   else
      Log('The hMailServer service, which an earlier run of setup set not to start, could not be set to start again (sc config hMailServer start= auto returned ' + IntToStr(ResultCode) +
          '); an administrator does so.');
end;

// Inno calls this after the last wizard page and before any file is installed, in a
// silent install too, and a non-empty answer ends the installation with that
// sentence (Setup's exit code 7). It is here and not in InitializeSetup because the
// settings file, which says whether this is a new installation, is found through
// {app}, which is not known until the directory has been chosen. Both installers:
// the database given for a new installation; the ARM64 one, below, what its server
// cannot use; and then, both again, the program folder, Bin and ControlPanel
// (PrepareProgramFolders, work item 78): a link on the way to them that anybody but
// the administrators controls, a share, a network drive or a volume without
// permissions refused, and otherwise the folder made with its permissions, or
// hardened, before any file is copied - the last step, after every refusal, since it
// changes the folder. What the folders' check found was mostly found on the Ready
// page, before the service was stopped (CheckProgramFolders); a refusal after setup
// stopped a running service says so, and how to start it again (PrepareToInstall).
function PrepareToInstallRefusal(): String;
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

   Result := PrepareProgramFolders(PreviousInstallationFolder());
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
   Result := PrepareToInstallRefusal();
   if Result <> '' then
   begin
      // Work item 78: a folder PrepareProgramFolders found changed since setup looked -
      // a link, a name taken, something put in it - which only something done while
      // another account could change it explains: the service set not to start, as
      // after the files, and the line hMailServer.Updater reads to put nothing back
      // over the folders (FoldersChangedLine). Otherwise the service was stopped on the
      // Ready page, before this: a refusal leaves it stopped, and says so - it is not
      // started again by setup, which has just found what it refused (NextButtonClick).
      if g_bFoldersChanged then
      begin
         KeepServiceFromStarting();
         Log(FoldersChangedLine());
         if g_bServiceStoppedForSetup then
            Result := Result + ' Setup stopped the hMailServer service for the installation and has set it not to start, since the folders it prepares were changed while it ' +
                      'prepared them: once what is said above has been seen to and Windows restarted, setup run again sets it to start again.';
      end
      else if g_bServiceStoppedForSetup then
         Result := Result + ' Setup stopped the hMailServer service for the installation and has not started it again: start it with "net start hMailServer" once what is said above has been seen to.';
      // A refusal whose remedy ends in a restart: not again before one (RecordRestartNeeded).
      if Pos('restarts Windows', Result) > 0 then
         RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), 'refused to install there for what a restart of Windows must follow (work item 78)');
      Log('Refused: ' + Result);
   end;
end;

function NextButtonClick(CurPage : Integer): boolean;
var
   hWnd: Integer;
   szIniFile : AnsiString;
   bWasRunning: Boolean;

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
		// Work item 78: the program folder, Bin and ControlPanel held and looked at,
		// changing nothing, before the service is stopped (CheckProgramFolders), so
		// that what setup refuses there leaves a running server running: Next goes on,
		// and PrepareToInstall gives the refusal (exit code 7 when silent) without the
		// service having been stopped. Inno Setup comes here in a silent installation
		// too, clicking Next for it.
		g_szFolderRefusal := CheckFoldersBeforeTheStop(PreviousInstallationFolder());
		if g_szFolderRefusal <> '' then
			Exit;

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
		// now bounded. Whether setup stopped a running service, which a refusal
		// after this says (PrepareToInstall), is set before the stop, whatever the
		// stop returns, and never cleared: a stop that timed out on one click may
		// still have stopped the service before the next, which then finds it
		// stopped.
		bWasRunning := IsServiceRunning('hMailServer');
		if bWasRunning then
			g_bServiceStoppedForSetup := true;
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

// For section_run.iss: whether the folders setup prepared were as it left them after
// the files (FoldersStillAsPrepared, CurStepChanged), so that the last page runs
// nothing of an installation they were not - its Control Panel entry, which Finish
// would run from ControlPanel, among it.
function FoldersWereAsPrepared(): Boolean;
begin
   Result := not g_bFoldersNotAsPrepared;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
   szWhy: String;
begin

	if CurStep = ssInstall then
	begin
	   // The program folder was made with its permissions, or hardened, in
	   // PrepareToInstall (PrepareProgramFolders), and is held until
	   // DeinitializeSetup.

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
	  // The files are in: a folder this setup made is the installation's now, and
	  // DeinitializeSetup leaves it (RemoveFoldersMadeHere).
	  g_bFilesInstalled := true;

	  // Work item 78: the folders setup prepared, read again before anything of the
	  // installation runs (FoldersStillAsPrepared). Not as setup left them, nothing runs:
	  // the service is kept from starting, setup is not run again before Windows has
	  // restarted (RecordRestartNeeded), the log has the line hMailServer.Updater reads
	  // to put nothing back over them (FoldersChangedLine), and the installation fails,
	  // exit code 12 (GetCustomSetupExitCode), the exception putting it in the log and on
	  // screen. As setup left them, the files setup kept in a folder it hardened in place
	  // go (ReleasePlaceholders), and a service an earlier run set not to start is set to
	  // start again (RestoreServiceStart).
	  if not FoldersStillAsPrepared(szWhy) then
	  begin
	    g_bFoldersNotAsPrepared := true;
	    g_bStepsIncomplete := true;
	    KeepServiceFromStarting();
	    RecordRestartNeeded(RemoveBackslashUnlessRoot(ExpandConstant('{app}')), 'found the folders it prepared not as it left them after the files (work item 78)');
	    Log(FoldersChangedLine());
	    RaiseException('Setup has copied the files, but the folders it prepared for them are not as it left them: ' + szWhy + 'Setup runs nothing of the installation, and an ' +
	                   'installed hMailServer service is set not to start. An administrator looks at what is named, restarts Windows, and runs setup again (work item 78).');
	  end;
	  ReleasePlaceholders();
	  RestoreServiceStart();

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
// without restoring the database". And 12 when the folders setup prepared were not as
// it left them after the files (work item 78, FoldersStillAsPrepared): the files are in,
// nothing of the installation ran, the service is set not to start, and nothing is to
// be put back over those folders - hMailServer.Updater never rolls back on 12
// (InstallerFoldersChanged), any more than on the 7 of a refusal whose log has
// FoldersChangedLine.
function GetCustomSetupExitCode(): Integer;
begin
	if g_bFoldersNotAsPrepared then
		Result := 12
	else if g_bStepsIncomplete and g_bDatabaseChanged then
		Result := 11
	else if g_bStepsIncomplete then
		Result := 10
	else
		Result := 0;
end;

// Inno calls this just before setup ends, whatever happened, and when nothing was
// installed too. When the files were not installed - setup cancelled, failed, or
// stopped before them - the folders this setup made that are still empty and still
// where it made them are removed, deepest first, while the folders above them are
// still held (RemoveFoldersMadeHere, work item 78's R1); then the folders
// PrepareProgramFolders held from the root of their volume down are let go.
procedure DeinitializeSetup();
begin
	ReleasePlaceholders();
	if not g_bFilesInstalled then
		RemoveFoldersMadeHere();
	ReleaseHeldFolders();
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


