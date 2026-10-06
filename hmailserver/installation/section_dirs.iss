[Dirs]
; The program folder first, which Inno Setup has just made: HardenNewProgramFolder
; (hMailServerInnoExtension.iss) gives a folder this installation made SYSTEM and the
; Administrators full control and Users Read & execute, inheriting nothing, before any
; other folder is made in it or any file copied - a folder at a drive's root would hand
; them every user's right to change them until hMailServer.exe /ProtectFolders runs
; after the files. Inno Setup's Permissions: parameter only adds entries, and cannot
; take away what the folder above hands down.
Name: "{app}"; AfterInstall: HardenNewProgramFolder
Name: "{app}\Data"; Components: server;
Name: "{app}\Database"; Components: server;
Name: "{app}\Logs"; Components: server;
Name: "{app}\Languages"
Name: "{app}\DBScripts"; Components: server;
Name: "{app}\Events"; Components: server;
Name: "{app}\Temp"; Components: server;
