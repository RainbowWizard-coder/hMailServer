[Setup]
; The ARM64 installer (hMailServerARM64.iss): the version is section_setup_64.iss's,
; read by the preprocessor there. arm64 only: an x64 machine cannot run the ARM64
; server, and hMailServer64.iss is the installer for it.
OutputBaseFilename=hMailServer-{#HM_APP_VERSION}-arm64
AppVerName=hMailServer {#HM_APP_VERSION}-ARM64
ArchitecturesInstallIn64BitMode=arm64
ArchitecturesAllowed=arm64
AppVersion={#HM_APP_VERSION}
VersionInfoVersion={#HM_VERSION_INFO}
; As section_setup_64.iss: the architecture the Control Panel's install checks.
VersionInfoDescription=hMailServer Setup (ARM64)
