[INI]
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "ProgramFolder"; String: "{app}";
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "DatabaseFolder"; String: "{app}\Database";  Flags: createkeyifdoesntexist; Components: server;
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "DataFolder"; String: "{app}\Data";  Flags: createkeyifdoesntexist; Components: server;
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "LogFolder"; String: "{app}\Logs"; Flags: createkeyifdoesntexist; Components: server;
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "TempFolder"; String: "{app}\Temp"; Flags: createkeyifdoesntexist; Components: server;
Filename: "{app}\Bin\hMailServer.INI"; Section: "Directories"; Key: "EventFolder"; String: "{app}\Events"; Flags: createkeyifdoesntexist; Components: server;

; Languages
Filename: "{app}\Bin\hMailServer.INI"; Section: "GUILanguages"; Key: "ValidLanguages"; String: "english,swedish";
; [Security] AdministratorPassword is not written from here any more. Until 25
; September 2026 an entry here wrote GetMD5OfString of the password: an unsalted MD5
; digest, which the server accepted and never re-hashed. The server makes the hash
; now - PBKDF2, the one the Control Panel writes - when RunPostInstallTasks runs
; hMailServer.exe --set-admin-password, with the password in the environment and
; never on its command line. See WriteAdministratorPassword in
; hMailServerInnoExtension.iss.
