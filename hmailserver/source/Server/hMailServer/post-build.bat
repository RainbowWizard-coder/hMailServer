@echo off
setlocal

set HMS_LIBS=%~1
set OUT_DIR=%~2
set TARGET=%~3
set INT_DIR=%~4
set SCRIPT_DIR=%~dp0
rem The fifth argument is the platform, given by the Release|ARM64 configuration
rem alone; the x64 configurations pass four and take the path they always took.
set HMS_PLATFORM=%~5

if /I "%HMS_PLATFORM%"=="ARM64" goto arm64

NET STOP hMailServer

xcopy /F /Y "%HMS_LIBS%\openssl-4.0.2\out64\bin\libcrypto-4-x64.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

xcopy /F /Y "%HMS_LIBS%\openssl-4.0.2\out64\bin\libssl-4-x64.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

xcopy /F /Y "%HMS_LIBS%\postgresql-18.3\builddir\src\interfaces\libpq\*.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

rem MySQL/MariaDB client (MariaDB Connector/C, shipped as libmysql.dll) + its auth
rem plugins, vendored in the repo. Copied here so MySQL/MariaDB regression tests can
rem load the same client the installer ships.
set MARIADB_CONNECTOR=%SCRIPT_DIR%..\..\..\..\libraries\mariadb-connector-c-3.4.9
xcopy /F /Y "%MARIADB_CONNECTOR%\libmysql.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

if not exist "%OUT_DIR%plugin\" mkdir "%OUT_DIR%plugin"
xcopy /F /Y "%MARIADB_CONNECTOR%\plugin\*.dll" "%OUT_DIR%plugin\"
if errorlevel 1 exit /b 1

rem MIDL generates the type library into the intermediate directory, but the
rem installer ships it from the output directory. Without this copy the
rem installer cannot be built from a clean checkout.
if not "%INT_DIR%"=="" (
   xcopy /F /Y "%INT_DIR%hMailServer.tlb" "%OUT_DIR%"
   if errorlevel 1 exit /b 1
)

rem The Control Deck is served from {program}\WebAdmin\index.html. The installer
rem ships it there; build.ps1 copies it too, because it runs with the post-build
rem events off. This copy serves builds made from the IDE.
if not exist "%OUT_DIR%WebAdmin\" mkdir "%OUT_DIR%WebAdmin"
xcopy /F /Y "%SCRIPT_DIR%..\..\..\installation\WebAdmin\index.html" "%OUT_DIR%WebAdmin\"
if errorlevel 1 exit /b 1
rem And its catalogues, one per language, served from the languages directory
rem beside the page.
if not exist "%OUT_DIR%WebAdmin\languages\" mkdir "%OUT_DIR%WebAdmin\languages"
xcopy /F /Y "%SCRIPT_DIR%..\..\..\installation\WebAdmin\languages\*.json" "%OUT_DIR%WebAdmin\languages\"
if errorlevel 1 exit /b 1

"%TARGET%" /Register
if errorlevel 1 exit /b 1
exit /b 0

:arm64
rem Windows on ARM (hmailserver\docs\WindowsOnArm.md): the ARM64 libraries, built
rem by libraries\build-*.ps1 -Platform ARM64 into their own -arm64 folders. No
rem MySQL client is copied: there is no ARM64 build of the one the x64 build
rem vendors, and an x64 DLL cannot load into an ARM64 process.
xcopy /F /Y "%HMS_LIBS%\openssl-4.0.2-arm64\out64\bin\libcrypto-4-arm64.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

xcopy /F /Y "%HMS_LIBS%\openssl-4.0.2-arm64\out64\bin\libssl-4-arm64.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

xcopy /F /Y "%HMS_LIBS%\postgresql-18.3-arm64\builddir\src\interfaces\libpq\*.dll" "%OUT_DIR%"
if errorlevel 1 exit /b 1

if not "%INT_DIR%"=="" (
   xcopy /F /Y "%INT_DIR%hMailServer.tlb" "%OUT_DIR%"
   if errorlevel 1 exit /b 1
)

if not exist "%OUT_DIR%WebAdmin\" mkdir "%OUT_DIR%WebAdmin"
xcopy /F /Y "%SCRIPT_DIR%..\..\..\installation\WebAdmin\index.html" "%OUT_DIR%WebAdmin\"
if errorlevel 1 exit /b 1
if not exist "%OUT_DIR%WebAdmin\languages\" mkdir "%OUT_DIR%WebAdmin\languages"
xcopy /F /Y "%SCRIPT_DIR%..\..\..\installation\WebAdmin\languages\*.json" "%OUT_DIR%WebAdmin\languages\"
if errorlevel 1 exit /b 1

rem An ARM64 hMailServer.exe runs only on an ARM64 Windows: built on an x64 machine
rem it is not started, so it is not registered (and the x64 server the machine may
rem have registered is not stopped). Built on an ARM64 machine it is, as the x64
rem build registers itself.
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" goto arm64register
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" goto arm64register
echo Not an ARM64 machine: the ARM64 server is not registered.
exit /b 0

:arm64register
NET STOP hMailServer
"%TARGET%" /Register
if errorlevel 1 exit /b 1
exit /b 0
