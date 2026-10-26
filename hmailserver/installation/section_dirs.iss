[Dirs]
; The program folder, and the folders above it that were not there, are made by setup
; itself in PrepareToInstall, before Inno Setup makes anything (PrepareProgramFolders and
; MakeFolders in hMailServerInnoExtension.iss): each with permissions of its own
; from the moment it is made - SYSTEM and the Administrators full control, Users Read &
; execute where every user could read a folder made there, nothing inherited, the
; Administrators the owner - so that no folder of the program ever carries what the
; folder above hands down. Inno Setup's Permissions: parameter only adds entries, and
; cannot take away what the folder above hands down. Inno Setup records for the
; uninstaller only a folder it made itself, so the entries below give it the ones setup
; made: uninsalwaysuninstall, which records a folder that is there, and a Check that is
; true only for a folder this installation made. The folders above the program folder
; come first, the shallowest first, and the program folder after them: the uninstaller
; goes through what it recorded backwards, and removes each when it is empty. Bin and
; ControlPanel follow, when setup made them, before Inno Setup made anything, in a program
; folder that was there (MakeFolders): with those permissions too, and not what the program
; folder hands down.
Name: "{code:FolderMadeBySetup|0}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(0)
Name: "{code:FolderMadeBySetup|1}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(1)
Name: "{code:FolderMadeBySetup|2}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(2)
Name: "{code:FolderMadeBySetup|3}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(3)
Name: "{code:FolderMadeBySetup|4}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(4)
Name: "{code:FolderMadeBySetup|5}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(5)
Name: "{code:FolderMadeBySetup|6}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(6)
Name: "{code:FolderMadeBySetup|7}"; Flags: uninsalwaysuninstall; Check: SetupMadeFolder(7)
Name: "{app}"; Flags: uninsalwaysuninstall; Check: SetupMadeProgramFolder
Name: "{app}\Bin"; Flags: uninsalwaysuninstall; Check: SetupMadeBin
Name: "{app}\ControlPanel"; Flags: uninsalwaysuninstall; Check: SetupMadeControlPanel
Name: "{app}\Data"; Components: server;
Name: "{app}\Database"; Components: server;
Name: "{app}\Logs"; Components: server;
Name: "{app}\Languages"
Name: "{app}\DBScripts"; Components: server;
Name: "{app}\Events"; Components: server;
Name: "{app}\Temp"; Components: server;
