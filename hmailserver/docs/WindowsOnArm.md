Windows on ARM
==============

hMailServer builds for 64-bit ARM Windows (ARM64) as well as for x64. The
server, its minidump writer and its update helper are cross-compiled on an x64
machine with Visual Studio's ARM64 tools. The same build links against ARM64
builds of OpenSSL, Boost and libpq. The Control Panel and the database tools are
published for `win-arm64`. The installer, `hMailServer-<version>-arm64.exe`,
comes from `hMailServerARM64.iss`, which uses the same section files as the x64
installer.

**This has not been run on ARM64 hardware.** It was written on 25 September 2026
on an x64 machine, and first compiled there on 26 September 2026: the three
libraries, the server, the tools and `hMailServer-6.3.4-arm64.exe` all built,
and every native image is an `AA64` machine image, but none of it has run. The project
has no ARM64 Windows machine, and no CI job builds it: the Windows runner,
bench150, is x64. The Roadmap2 row "Windows on ARM" stays in motion until an
ARM64 build has been installed and has run the regression suite on real
hardware. Until then, read every "works" below as "is built to work".

The x64 build is unchanged. `build.ps1` and every other script default to x64,
the project files gained a `Release|ARM64` configuration beside the existing
ones, and `hMailServer64.iss` compiles to the same installer it did before.

What the ARM64 server can use
-----------------------------

| Part | On ARM64 | Why |
|---|---|---|
| The server, SMTP, IMAP, POP3, the web listeners | Yes, native | The same source, compiled for ARM64. Nothing in it is x64-specific: no inline assembly and no x64 intrinsics. |
| The built-in database (SQL Server Compact) | **No** | Microsoft never built SQL Server Compact for ARM64, and it is out of support. The x64 runtime installs on Windows 11 on ARM, but its OLE DB provider is an in-process x64 DLL. An ARM64 process cannot load an x64 DLL (only an ARM64EC process can), so the ARM64 `hMailServer.exe` cannot open a `.sdf` file. |
| Microsoft SQL Server | Yes | Through the Microsoft OLE DB Driver 19 for SQL Server, which has an ARM64 installer since 19.3.0. Install the ARM64 driver, not the x64 one: the x64 driver is for x64 processes only. Windows' own SQLOLEDB, the fallback when no driver is installed, should be native on Windows on ARM, but this has not been checked. |
| PostgreSQL | Yes | libpq is built for ARM64 against the ARM64 OpenSSL and is installed with the server. |
| MySQL and MariaDB | **Only with a client you build** | The server loads `libmysql.dll` (MariaDB Connector/C) at run time. MariaDB publishes no Windows ARM64 build of it, and the x64 one cannot load into an ARM64 process, so the ARM64 installer carries none. See [A MySQL client for ARM64](#a-mysql-client-for-arm64). |
| Backup and restore | Yes | The installer carries 7-Zip's own ARM64 console, `arm64/7za.exe` from the same 7-Zip Extra 26.03 package as the x64 one. |
| The Control Panel, DBSetup, DBSetupQuick, DBUpdater, the add-ons | Yes, native | They are .NET 10 applications, published for `win-arm64`. The installer carries the ARM64 .NET 10 Desktop Runtime and installs it when it is missing. |
| The C++ runtime | Yes | The ARM64 build of the v145 runtime, gathered from Visual Studio's `arm64` redistributable folder. |
| Live update | Yes, by name | An ARM64 server looks for `hMailServer-<version>-arm64.exe` in a release (`UpdateChecker`), and the feed generator knows that name. No release carries one yet, so until one does, the check says that the release has no ARM64 installer. |
| `VersionArchitecture` (COM), the status page, the metrics label, SNMP | Say `ARM64` | They said `x64` for every 64-bit build. |

The service account, the settings (in the database, as on x64), the Control
Deck, the webmail and every other feature do not depend on the processor. No
setting is new, and there is no ini key.

### A new installation

The installer offers no built-in database. The choice on its database page is
disabled and says why, and `/useinternaldbms` is ignored. A new installation
runs DBSetup, which sets the server up against Microsoft SQL Server, PostgreSQL
or (with your own client) MySQL or MariaDB. DBSetup is a wizard, so **a new
ARM64 installation cannot finish unattended** the way an x64 installation on
the built-in database can. The x64 installer never met this, because its silent
default is the built-in database.

### An upgrade

The ARM64 installer keeps the x64 installer's AppId, so it installs over an
existing installation in the same folder. That is also how an x64 installation
is moved to the ARM64 build on an ARM64 machine. Before any file is replaced
(`PrepareToInstall`):

- a server on the built-in database is refused, with a message that says so and
  changes nothing. Move its data to SQL Server or PostgreSQL first with the x64
  build ([MigratingDatabaseBackend.md](MigratingDatabaseBackend.md)), then run the
  ARM64 installer;
- a server on MySQL or MariaDB is warned that it needs an ARM64 `libmysql.dll`.
  The installation goes on. A `libmysql.dll` an x64 installation left in `Bin` is
  the x64 one, and it does not load.

### Running the x64 build under emulation instead

Windows 11 on ARM runs x64 programs under emulation. There, the x64 server and
the x64 SQL Server Compact provider are both x64 code in one x64 process, so the
built-in database would work. The x64 installer does not allow this today:
`ArchitecturesAllowed=x64` in `section_setup_64.iss` means an x64 Windows in the
Inno Setup 6 that builds it, and it refuses an ARM64 machine. Changing it to
`x64compatible` would let the x64 installer run there. That is a decision for
the owner, not part of this row: it would ship a configuration nobody has tested
on the product's main installer.

Building it
-----------

Everything below runs on an x64 machine. Nothing has to run on ARM64 hardware
until the result is tested.

### Visual Studio

The server is an ATL project. MSBuild, the ARM64 compilers and ATL for ARM64
must be in **one** Visual Studio 2026 instance:

- `Microsoft.VisualStudio.Component.VC.Tools.ARM64`: "MSVC v145 - ARM64/ARM64EC build tools";
- `Microsoft.VisualStudio.Component.VC.ATL.ARM64`: "C++ ATL for v145 build tools (ARM64/ARM64EC)".

`build.ps1 -Platform ARM64` looks only for an instance that has both. It stops
with the two ids if none has them. On the development machine on 25 September
2026, Build Tools 18 had ATL for x64 but no ARM64 tools, and Visual Studio 18
Insiders had the ARM64 tools, the ARM64 runtime and `vcvarsamd64_arm64.bat`,
but no ATL for any processor. Adding the two components to the Build Tools
instance, from an elevated prompt, makes one instance that has everything:

    "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify ^
      --installPath "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" ^
      --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 ^
      --add Microsoft.VisualStudio.Component.VC.ATL.ARM64 --passive --norestart

The Windows SDK (10.0.26100) already has its ARM64 libraries (`um\arm64`,
`ucrt\arm64`).

The libraries need only the ARM64 compilers, not ATL, so they can be built from
the Insiders instance before the change above.

### The libraries

Each script takes `-Platform ARM64` and builds into a tree of its own beside the
x64 one. It never deletes or touches the x64 tree. The trees are
`%hMailServerLibs%\openssl-4.0.2-arm64`, `boost_1_92_0-arm64` and
`postgresql-18.3-arm64`, with the same layout inside as the x64 trees:

    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.2 -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-boost.ps1 -Version 1.92.0 -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-pgsql.ps1 -Version 18.3 -OpenSSLVersion 4.0.2 -Platform ARM64

In order, because libpq links against the ARM64 OpenSSL:

- **OpenSSL** is configured for `VC-WIN64-ARM` with `no-asm`, as the x64 build
  is configured for `VC-WIN64A`, in the `vcvarsamd64_arm64.bat` environment. It
  makes `libcrypto-4-arm64.dll` and `libssl-4-arm64.dll`, a name that comes
  from OpenSSL's own `multilib` suffix. This is expected to take 15 to 25
  minutes, because nmake is single-threaded, as it is for x64.
- **Boost** bootstraps b2 in the x64 environment, because b2 has to run on this
  machine. It then builds with `architecture=arm address-model=64`, and b2 sets
  up the cross compiler itself. The libraries are named with `-a64`, for example
  `libboost_thread-vc145-mt-a64-1_92.lib`, which is what Boost's auto-linking
  asks for when `_M_ARM64` is defined. This is expected to take 5 to 15 minutes.
- **libpq** is built with Meson and a cross file that the script writes into
  the tree (`hmailserver-arm64.cross`, `cpu_family = 'aarch64'`). The cross file
  is needed because Meson cannot run what the cross compiler makes. The check at
  the end requires `libpq.dll` to import `libssl-4-arm64.dll`. This is expected
  to take 3 to 10 minutes.

The logs are `libraries\build-*-arm64.log`. The first run, on 26 September 2026
on a 32-thread x64 machine, took 16.5 minutes for OpenSSL, 2.5 minutes for Boost
(`-Jobs 16`) and under a minute for libpq. libpq's cross build needs
win_flex and win_bison on PATH, as the x64 one does, and is given
`-Dsystem_tzdata`: a Meson cross build otherwise stops for want of a `zic` that
runs on the build machine, and libpq uses no time zone data.

### The server, the tools and the installer

    powershell -NoProfile -ExecutionPolicy Bypass -File build\build.ps1 -Configuration Release -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File build\build-tools.ps1 -Configuration Release -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File build\get-installer-binaries.ps1 -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File build\get-dotnet-runtime.ps1 -Platform ARM64
    "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" hmailserver\installation\hMailServerARM64.iss

- `build.ps1 -Platform ARM64` builds `Release|ARM64` into
  `hmailserver\source\Server\hMailServer\ARM64\Release`. It copies in the Control
  Deck, the type library and the ARM64 OpenSSL and libpq DLLs, and it logs to
  `logs\build-Release-ARM64.log`. It takes about as long as the x64 Release
  build. The post-build step of an IDE build does the same copies. It registers
  the server only on an ARM64 machine, because an ARM64 `hMailServer.exe` does
  not start on x64.
- `build-tools.ps1 -Platform ARM64` publishes DBSetup, DBSetupQuick, DBUpdater,
  the two add-ons and the Control Panel for `win-arm64`, into `publish-arm64`
  beside each project's `publish`. The ARM64 publish has its own `bin\ARM64` and
  `obj\ARM64`, so a later x64 publish cannot pick up ARM64 assemblies. It also
  has its own NuGet lock files, `packages.arm64.lock.json`, so that it never
  rewrites the `win-x64` lock files that CI restores in locked mode. The first
  ARM64 publish creates them. Commit them with the first ARM64 build.
  The tools compile against, and the installer ships, a COM wrapper of their
  own, `Interop\ARM64\Interop.hMailServer.dll`: `generate-com-wrapper.ps1
  -Platform ARM64` makes it with TlbImp `/machine:Agnostic`, and
  `Tools\Directory.Build.targets` points the reference at it for this publish
  only. TlbImp has no ARM64 machine type, and the x64 wrapper is marked x64,
  which .NET refuses to load into an ARM64 process (BadImageFormatException at
  the first use of the COM API). The first ARM64 build, on 26 September 2026,
  found this from the compiler's warning CS8012.
- `get-installer-binaries.ps1 -Platform ARM64` places what
  `third-party-binaries.json` marks `"platform": "arm64"`, under
  `hmailserver\installation\ARM64`. That is the ARM64 C++ runtime, gathered from
  `%VCToolsRedistDir%\arm64` of the instance with the ARM64 tools, and 7-Zip's
  ARM64 console, taken from the same pinned archive as the x64 console. Without
  `-Platform` the script does what it always did, and it skips these entries.
- `get-dotnet-runtime.ps1 -Platform ARM64` downloads the `win-arm64` .NET
  Desktop Runtime 10.0.12 and checks it against the SHA-512 that Microsoft
  publishes for it, which is pinned in the manifest beside the x64 runtime.
- `hMailServerARM64.iss` reads the version from `section_setup_64.iss`, so the
  release's version stamp is still one installer file.

Why a second installer and not one for both
-------------------------------------------

Every native file differs between the two builds: the server, the minidump
writer, the updater, OpenSSL, libpq, the C++ runtime, 7-Zip and the .NET
runtime. The .NET tools differ too. The x64 installer also carries SQL Server
Compact and the MySQL client, and ARM64 has neither. One installer for both
would carry every file twice, with a `Check:` on every entry, and a mistake in
one of those checks would install x64 files on ARM64 or ARM64 files on x64.
Two installers share everything else through the section files and
`hMailServerInnoExtension.iss`, where `#ifdef HM_ARM64` marks the places where
they differ. They also stay two separate files for the update check to find by
name: the server asks for the installer of its own processor. Above all, two
installers keep the x64 installer, the one the smoke test and every release
depend on, exactly as it was.

A MySQL client for ARM64
------------------------

MariaDB Connector/C builds for ARM64 with CMake and Visual Studio. The project
does not build or ship it, because nothing here could test it. To use MySQL or
MariaDB on an ARM64 server:

1. Build Connector/C 3.4.9 from MariaDB's source archive for `-A ARM64`
   (`cmake -A ARM64 -DCMAKE_BUILD_TYPE=Release`, then build `libmariadb`), in
   the `vcvarsamd64_arm64.bat` environment.
2. Copy `libmariadb.dll` to `hMailServer\Bin\libmysql.dll`, and the
   authentication plugins to `Bin\plugin`, as the x64 installer lays them out.
3. Start the service. If the client is missing or is the x64 one, the ERROR log
   says so (HM5094, "The MySQL client (libmysql.dll, ARM64) could not be
   loaded").

What a first run on ARM64 hardware has to show
----------------------------------------------

The row is done when someone has done the following on an ARM64 Windows 11
machine and written down the result:

1. Build the libraries and the server as above, and confirm with
   `dumpbin /headers` that `hMailServer.exe`, both OpenSSL DLLs and `libpq.dll`
   are `AA64` (ARM64) machine images.
2. Install `hMailServer-<version>-arm64.exe` on a clean machine against
   PostgreSQL. Start the service, open the Control Panel and send and receive a
   message.
3. Run the regression suite against that server on PostgreSQL or SQL Server.
   It cannot use the built-in database, so the fixtures that assume it are
   skipped or pointed at the external database, as on the second bench.
4. Take a backup and restore it (the ARM64 7-Zip).
5. Install it over an x64 installation that uses SQL Server Compact, and see it
   refused with nothing changed.

Until then this page, `section_files_arm64.iss` and the libraries' ARM64 paths
are a build that has never run.
