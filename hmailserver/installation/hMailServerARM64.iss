; The installer for Windows on ARM (hmailserver/docs/WindowsOnArm.md): the ARM64
; build of the server, its tools and its libraries, with the section files the x64
; installer (hMailServer64.iss) uses. HM_ARM64 is what the shared files test where
; the two differ: the files the x64 installer carries that have no ARM64 build, and
; the built-in database, which the ARM64 server cannot use - SQL Server Compact
; has no ARM64 build, and an ARM64 process cannot load an x64 OLE DB provider.
#define HM_ARM64
#define HMAILSERVERLIBS = GetEnv("hMailServerLibs")
#define OPENSSL_LIBS_PATH HMAILSERVERLIBS + "\openssl-4.0.3-arm64\out64\bin"
#define POSTGRESQL_LIBPQ_PATH HMAILSERVERLIBS + "\postgresql-18.6-arm64\builddir\src\interfaces\libpq"

; As in hMailServer64.iss, which says why these are defined once: the ARM64 build
; of the same .NET Desktop Runtime release (third-party-binaries.json pins both,
; and build\get-dotnet-runtime.ps1 -Platform ARM64 places this one).
#define DOTNET_MAJOR "10"
#define DOTNET_CHANNEL "10.0"
#define DOTNET_RUNTIME_FILE "windowsdesktop-runtime-" + DOTNET_CHANNEL + "-win-arm64.exe"

; The version is read from section_setup_64.iss, so that the release's version
; stamp (RELEASE.md step 7) stays one file for the two installers.
#define HM_APP_VERSION ReadIni(SourcePath + "\section_setup_64.iss", "Setup", "AppVersion", "")
#define HM_VERSION_INFO ReadIni(SourcePath + "\section_setup_64.iss", "Setup", "VersionInfoVersion", "")
#if HM_APP_VERSION == ""
  #error section_setup_64.iss names no AppVersion for the ARM64 installer to take
#endif

#include "section_setup.iss"
#include "section_setup_arm64.iss"
#include "section_custom_messages.iss"
#include "section_languages.iss"
#include "section_istool.iss"
#include "section_types.iss"
#include "section_components.iss"

#include "section_files_common.iss"

#include "section_files_arm64.iss"

#include "section_messages.iss"
#include "section_ini.iss"
#include "section_registry.iss"
#include "section_dirs.iss"
#include "section_run.iss"
#include "section_uninstallrun.iss"

#include "section_icons.iss"

#include "hMailServerInnoExtension.iss"
