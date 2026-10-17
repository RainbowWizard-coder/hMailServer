[Setup]
OutputBaseFilename=hMailServer-6.3.7-x64
AppVerName=hMailServer 6.3.7-x64
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
AppVersion=6.3.7
; VersionInfoVersion is <version>.0 on purpose. The build number (HMAILSERVER_BUILD in
; Version.h) travels in Version.h and the release tag; the installer and the seven .csproj
; files carry the version alone, so a build-only re-cut never has to touch them. RELEASE.md, step 7.
VersionInfoVersion=6.3.7.0
; The architecture in the installer's own file description, which an
; administrator's install from the Control Panel reads before it runs one
; (Common/Util/AdminUpdateChecks.h): "(x64)" here, "(ARM64)" in the ARM64
; installer. An installer without it is taken for x64, as every Windows release
; before 6.3.6 was.
VersionInfoDescription=hMailServer Setup (x64)
