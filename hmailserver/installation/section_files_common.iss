[Files]
Source: "License.rtf"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server admintools;

; 3'rd party dependencies. None of these is in git: build\get-installer-binaries.ps1
; fetches or gathers each one and verifies it against hmailserver\docs\third-party-binaries.json,
; and the installer smoke test proves the result installs. atl70.dll (ATL 7.0, Visual
; Studio .NET 2003) is no longer shipped: nothing on a v145 build links it, and the
; smoke test on a clean runner is what says so.
;
; HM_ARM64 is defined by hMailServerARM64.iss alone: the ARM64 installer carries its
; own 7-Zip console and no MySQL client (section_files_arm64.iss says why). For
; hMailServer64.iss these lines are what they always were.
#ifndef HM_ARM64
Source: ".\Extras\7za.exe"; DestDir: "{app}\Bin"; Flags: ignoreversion; Components: server;
#endif

; Database scripts
Source: "..\source\DBScripts\*.sql"; DestDir: "{app}\DBScripts";Flags: ignoreversion recursesubdirs; Components: server;

; onlyifdoesntexist keeps an upgrade from touching user-modified files, and
; uninsneveruninstall keeps the uninstaller from deleting them: without it the
; uninstall log entry recorded on first install removes the administrator's
; customized EventHandlers.vbs (and addon scripts) on uninstall/reinstall.
Source: "..\source\Addons\*.*"; DestDir: "{app}\Addons"; Flags: onlyifdoesntexist recursesubdirs uninsneveruninstall; Excludes: "Events";  Components: server;
Source: "..\source\Addons\Events\*.*"; DestDir: "{app}\Events"; Flags: onlyifdoesntexist uninsneveruninstall; Components: server;

Source: "..\source\Translations\*"; Excludes: "CVS,.cvsignore,.#*"; DestDir: "{app}\Languages"; Components: server admintools;

Source: ".\Extras\dh2048.pem"; DestDir: "{app}\Bin";  Flags: ignoreversion; Components: server;

#ifndef HM_ARM64
; MySQL/MariaDB client library (x64) - MariaDB Connector/C 3.4.11, loaded at runtime as
; libmysql.dll when the database type is MySQL/MariaDB. Talks to both MySQL and MariaDB servers.
Source: ".\Extras\libmysql.dll"; DestDir: "{app}\Bin"; Flags: ignoreversion uninsrestartdelete; Components: server;
; Its seven authentication plugins, found via MYSQL_PLUGIN_DIR = {app}\Bin\plugin, so that the one
; client authenticates every common account type: caching_sha2_password (MySQL 8.0+ default),
; sha256_password, client_ed25519 / auth_gssapi_client / parsec (MariaDB), dialog, mysql_clear_password.
; Named one by one rather than .\Extras\plugin\*.dll: build\get-installer-binaries.ps1 places files
; and removes none, so a checkout that once fetched an earlier client keeps that client's other
; files in Extras\plugin, and a wildcard would ship them. build\check-installer-scripts.py holds
; these names to the manifest's Extras/plugin entries.
Source: ".\Extras\plugin\auth_gssapi_client.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\caching_sha2_password.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\client_ed25519.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\dialog.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\mysql_clear_password.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\parsec.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
Source: ".\Extras\plugin\sha256_password.dll"; DestDir: "{app}\Bin\plugin"; Flags: ignoreversion uninsrestartdelete; Components: server;
#endif

#ifndef HM_ARM64
; Two files 6.2.3 to 6.3.6 put in Bin\plugin beside the client's plugins, which this
; installer no longer carries (ThirdPartyBinaries.md): auth_named_pipe.dll, MariaDB Server's
; own named_pipe authentication plugin, which no client loads, and
; authentication_windows_client.dll, the client of MySQL Enterprise Edition's Windows
; authentication. Both came from MariaDB Server's tree, not Connector/C, under the GPL. An
; upgrade removes them, so that Bin\plugin holds what the installer says it does.
[InstallDelete]
Type: files; Name: "{app}\Bin\plugin\auth_named_pipe.dll"
Type: files; Name: "{app}\Bin\plugin\authentication_windows_client.dll"
#endif
