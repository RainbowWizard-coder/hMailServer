[Files]
; The ARM64 installer's files (hMailServerARM64.iss; hmailserver/docs/WindowsOnArm.md).
; section_files_64.iss's, from the ARM64 build: build.ps1 -Platform ARM64, the
; libraries built with libraries\build-*.ps1 -Platform ARM64, the tools and the
; Control Panel published by build\build-tools.ps1 -Platform ARM64, and the
; third-party files build\get-installer-binaries.ps1 -Platform ARM64 and
; build\get-dotnet-runtime.ps1 -Platform ARM64 put under ARM64\.
;
; uninsrestartdelete on every file the server's process loads, for the reason
; section_files_64.iss gives.
;
; Main server
Source: "..\source\server\hMailServer\ARM64\Release\hMailServer.exe"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;
Source: "..\source\server\hMailServer\ARM64\Release\hMailServer.tlb"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server admintools;
Source: "..\source\server\hMailServer\ARM64\Release\hMailServer.Minidump.exe"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server;
Source: "..\source\server\hMailServer\ARM64\Release\hMailServer.Updater.exe"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server;
; The ARM64 build of the Visual C++ runtime matching the toolset (v145).
Source: "ARM64\Microsoft.VC145.CRT\*"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;

; The Control Deck and its catalogues, as for x64.
Source: "WebAdmin\index.html"; DestDir: "{app}\WebAdmin"; Flags: ignoreversion; Components: server;
Source: "WebAdmin\languages\*.json"; DestDir: "{app}\WebAdmin\languages"; Flags: ignoreversion; Components: server;

; hMailServer Control Panel, published for win-arm64.
Source: "..\source\Tools\ControlPanel\publish-arm64\*"; DestDir: "{app}\ControlPanel"; Flags: ignoreversion recursesubdirs; Components: controlpanel;
; The ARM64 .NET {#DOTNET_MAJOR} Desktop Runtime, installed silently when missing,
; exactly as section_files_64.iss's x64 one.
Source: "ARM64\DotNet\{#DOTNET_RUNTIME_FILE}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Components: server controlpanel;

; No SQL Server Compact runtime: Microsoft never built one for ARM64, and the x64
; runtime's OLE DB provider cannot be loaded by an ARM64 hMailServer.exe. The
; built-in database is not offered (hMailServerInnoExtension.iss, HM_ARM64).

; The database tools and the add-ons, published for win-arm64.
Source: "..\source\Tools\DBUpdater\publish-arm64\*"; DestDir: "{app}\Bin"; Flags: ignoreversion recursesubdirs; Components: server;
Source: "..\source\Tools\DBSetup\publish-arm64\*"; DestDir: "{app}\Bin"; Flags: ignoreversion recursesubdirs; Components: server;
Source: "..\source\Tools\DBSetupQuick\publish-arm64\*"; DestDir: "{app}\Bin"; Flags: ignoreversion recursesubdirs; Components: server;
Source: "..\source\Tools\Interop\Interop.hMailServer.dll"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server admintools;
Source: "..\source\Tools\DataDirectorySynchronizer\publish-arm64\*"; DestDir: "{app}\Addons\DataDirectorySynchronizer"; Flags: ignoreversion recursesubdirs; Components: server;
Source: "..\source\Tools\ImportTool\publish-arm64\*"; DestDir: "{app}\Addons\ImportTool"; Flags: ignoreversion recursesubdirs; Components: server;

; 7-Zip's ARM64 console, from the same 7-Zip Extra package as the x64 one.
Source: "ARM64\Extras\7za.exe"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server;

; No MySQL/MariaDB client. MariaDB publishes no Windows ARM64 build of Connector/C,
; and the x64 libmysql.dll cannot load into an ARM64 process. An administrator who
; uses MySQL or MariaDB builds the client for ARM64 and puts libmysql.dll (and its
; plugin directory) in {app}\Bin; WindowsOnArm.md says how. Nothing here removes one
; that is there.

; OpenSSL, ARM64 (VC-WIN64-ARM)
Source: "{#OPENSSL_LIBS_PATH}\libcrypto-4-arm64.dll"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;
Source: "{#OPENSSL_LIBS_PATH}\libssl-4-arm64.dll"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;

; PQSQL (PostgreSQL client), ARM64
Source: "{#POSTGRESQL_LIBPQ_PATH}\*.dll"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server admintools;
