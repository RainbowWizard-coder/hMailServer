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
has no ARM64 Windows machine. The CI job `windows-packages-arm64` cross-compiles it
on bench150, which is x64, in every pipeline of `master`, a `batch*` branch and a `v*`
tag, and runs none of it. Up to 6.3.5 it stopped at its toolchain check, in 6.3.5's
tag pipeline too, and was let fail: the check looked for Visual Studio's ARM64 C++
runtime by a path that finds it in no installation, while bench150 has the ARM64
tools, ATL and SDK libraries. With the check corrected (the Unreleased section of
CHANGELOG.md), the job's own script ran whole on bench150 by hand, as LocalSystem,
on 29 September 2026: it built the three libraries into the bench's cache, the
server, the tools and the installer, and found every native file the installer
packs to be an ARM64 image but Microsoft's ARM64EC `vcruntime140_1.dll`, which it
allows ([PackagesWindows.md](PackagesWindows.md), "What was run by hand on
bench150"). The job may no longer fail, and from 6.3.6 a release
carries the ARM64 installer, Authenticode-signed, with its SBOM and its Sigstore
bundle. The Roadmap2 row "Windows on ARM" stays in motion until an ARM64 build has
been installed and has run the regression suite on real hardware. Until then, read
every "works" below as "is built to work", and [What is proven, and what needs ARM64
hardware](#what-is-proven-and-what-needs-arm64-hardware) for what has been shown on x64.

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
| Live update | Yes, by name | An ARM64 server looks for `hMailServer-<version>-arm64.exe` in a release (`UpdateChecker`), and the feed generator knows that name. No release up to 6.3.5 carries one, so for those the check says that the release has no ARM64 installer; from 6.3.6 a release carries it with its Sigstore bundle. An x64 server never takes it: it looks for `-x64.exe` by name. |
| `VersionArchitecture` (COM), the status page, the metrics label, SNMP | Say `ARM64` | They said `x64` for every 64-bit build. |

The service account, the settings (in the database, as on x64), the Control
Deck, the webmail and every other feature do not depend on the processor. No
setting is new, and there is no ini key.

### A new installation

The installer offers no built-in database. The choice on its database page is
disabled and says why, and `/useinternaldbms` is ignored. A new installation is
set up against Microsoft SQL Server, PostgreSQL or (with your own client) MySQL
or MariaDB, in one of two ways:

- the database given on the command line or in the environment is set up by
  `DBSetup.exe /unattended`, with no window at all
  ([A new installation without a window](#a-new-installation-without-a-window)).
  This is how a silent installation finishes;
- with none given, DBSetup's wizard asks, as it always has.

A silent new installation that is given no database is refused before anything
is installed, with a message that names the switches. The wizard is a window,
and a silent installation has nobody to answer it: under `/VERYSILENT` it would
wait for ever. The x64 installer never met this, because its silent default is
the built-in database.

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
built-in database would work. **The x64 installer does not allow this, by the
maintainers' decision of 28 September 2026, which the owner may overrule.**
`ArchitecturesAllowed=x64` in `section_setup_64.iss` stays as it is. Inno Setup
6.3 and later read `x64` as `x64os`, an x64 Windows, and so refuse an ARM64
machine (ISCC says the spelling is deprecated and substitutes `x64os`).
`x64compatible` would let the x64 installer run on Windows 11 on ARM. The reasons
it does not:

- Nobody has run the x64 server under emulation. The x64 installer is the one
  every release carries and the installer smoke test holds to its promises, and
  allowing it there would ship a configuration nobody has tested on the
  product's main installer.
- An x64 installation on an ARM64 machine would stay one. Its live update looks
  for the `-x64.exe` installer, and a later move to the ARM64 build means moving
  its built-in database to SQL Server or PostgreSQL by hand first: the ARM64
  installer refuses an installation on the built-in database.
- The ARM64 installer is the way onto ARM64. Until it has run on hardware, the
  answer for an ARM64 machine is "not yet", not a second untested way.

To overrule it, change `x64` to `x64compatible` in `ArchitecturesAllowed` (and
`ArchitecturesInstallIn64BitMode`) in `section_setup_64.iss`, and run the x64
installer and the regression suite on an ARM64 machine before a release carries
it.

A new installation without a window
-----------------------------------

Both installers take the external database a new installation is set up
against on the command line or in the environment. `DBSetup.exe /unattended`
then sets it up with no window at all. On ARM64, which has no built-in
database, this is how a silent installation finishes. On x64 it is the same
option beside the built-in database. The names are the container's
([EnvironmentConfiguration.md](EnvironmentConfiguration.md)), and each switch is
its variable's value on the command line, and wins over the variable:

| Switch | Variable | What it is |
|---|---|---|
| `/dbtype=` | `HM_DB_TYPE` | `PostgreSQL`, `MySQL` (or `MariaDB`) or `MSSQL`. Given, the database is external: the database page is skipped, and so is SQL Server Compact. |
| `/dbhost=` | `HM_DB_HOST` | The database server. For SQL Server, `HOST`, `HOST\INSTANCE` or `HOST,PORT`. |
| `/dbport=` | `HM_DB_PORT` | PostgreSQL's (5432 when not given) and MySQL's (3306). Not for SQL Server, which takes its port in the host. |
| `/dbname=` | `HM_DB_NAME` | The database, `hMailServer` when not given. A database DBSetup creates is named with letters, digits and underscores. |
| `/dbuser=` | `HM_DB_USER` | The user. Required for PostgreSQL and MySQL. For SQL Server, none means Windows authentication (below). |
| none | `HM_DB_PASSWORD` | The password. It is never a switch. |
| `/dbpasswordfile=` | `HM_DB_PASSWORD_FILE` | The full path of a file that holds the password, as UTF-8 text (or UTF-16 with its byte order mark); a file in another encoding is refused. The line breaks that end it, and a UTF-8 byte order mark, are not part of it. |
| `/createdatabase=` | `HM_CREATE_DATABASE` | `1` (the default) creates the database. `0` uses one that exists, already hMailServer's. |
| `/dbservice=` | `HM_DB_SERVICE` | The Windows service of a database server on this machine, by its name (`postgresql-x64-18`, `MSSQL$SQLEXPRESS`), for hMailServer to start after. |

For example, from an elevated prompt:

    hMailServer-6.3.6-arm64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART ^
      /dbtype=PostgreSQL /dbhost=db1.example.com /dbuser=hmailserver ^
      /dbpasswordfile=C:\Deploy\hmailserver-db.txt

The administrator password is given as for any silent installation, in
`HM_ADMIN_PASSWORD` or with `/adminpassword=`, and `/serviceaccount=` and
`/profile=` mean what they always did. An attended installation takes the
database the same way: with `/dbtype` or `HM_DB_TYPE` it skips its database page
and runs no wizard, so a variable left set in the environment counts there too:
an `HM_DB_TYPE` set in the machine's or the user's environment for anything else
makes every new installation on that machine, silent or not, an external one.
The setup log says which it was: `given with /dbtype` or `given with HM_DB_TYPE
in the environment`.

What happens, in order:

1. Before anything is installed, a new installation refuses a type it does not
   know and a `/dbpasswordfile` given by a relative path. (An upgrade keeps the
   database it has, so what it was given is not checked.) On ARM64 it also refuses
   MySQL or MariaDB unless a `libmysql.dll` is already in `Bin`, which has to
   be an ARM64 one ([A MySQL client for ARM64](#a-mysql-client-for-arm64)).
2. The files, the service and the administrator password, as always.
3. `DBSetup.exe /unattended`, with each switch given in its variable. It checks
   every setting before it contacts the server, and a setting it refuses
   changes nothing. It signs in as the administrator, sets the service
   dependency, then creates the database or names the one that exists.
   Creating makes the tables and marks the database as a new installation's.
   The server then restarts on the database, and DBSetup asks it which schema
   it opened. An existing database older than this build is upgraded by
   DBUpdater, as DBSetupQuick does for an upgrade. It ends at the schema the
   build requires, or with a failure.
4. As for any new installation: the secure defaults (only on a database DBSetup
   created, [SecureDefaults.md](SecureDefaults.md)), the profile, the
   stored-secrets key, the move to `NT SERVICE\hMailServer` - on SQL Server with
   Windows authentication after that account's login (below) - and the service's
   start, held to its opening the database when it was moved.

DBSetup's lines go into the setup log (`/LOG=`). When it fails, its words are
the message, which `/SUPPRESSMSGBOXES` answers, and Setup exits with a non-zero
code (10). DBSetup's own exit codes are 0 (set up, at the build's schema), 1 (the
step failed: the database server's answer is in the message), 2 (a setting was
refused, and nothing was changed), 3 (the administrator password was refused)
and 4 (run by hand to finish a new installation, below: the database is set up,
and a step after it did not complete, which its lines say). An upgrade keeps the
database it has: a database given to it is written to the setup log and ignored.

A new installation whose database step does not complete has nothing that comes
after that step either - the secure defaults, the profile, the stored-secrets key,
the move - and Setup leaves the mark that lets it be finished
([ServiceAccount.md](ServiceAccount.md), "A new installation whose database step
did not complete"): correct the setting and **run Setup again**, which sets the
installation up as a new one, or **run `DBSetup.exe /unattended` by hand** (below),
which after the database runs the steps Setup would have.

**The password.** The installer never reads the database password. DBSetup
reads it from `HM_DB_PASSWORD` or from the file, and hands it to the server,
which keeps it in `hMailServer.ini` under this machine's DPAPI, as it does for
the wizard. When DBSetup `/unattended` has finished, Setup empties every
variable of the database step from its own environment, so that no process it
starts after that inherits them. Variables reach Setup only when it is started
elevated, from an elevated prompt or by a deployment tool. A Setup started
without elevation asks for it, and Windows gives the elevated copy a fresh
environment, without the variables set in the prompt. Switches and a password
file get through either way.

**What the server passes on, and what it cannot.** From 6.3.6 the server passes
the host, the user and the password on as they are given, whatever they hold.
PostgreSQL's connection string has each value in single quotes with every single
quote and backslash in it escaped, as libpq reads them. A value with a letter
beyond U+00FF goes in UTF-8, where until 6.3.6 the whole string was left empty
and libpq connected with its defaults; one whose letters are all U+00FF or below
goes in Latin-1, as it always did, so that a password with such a letter that
connects today - set on a PostgreSQL server whose encoding is WIN1252 or LATIN1 -
goes on connecting. (A role on a server in UTF8 whose password has such a letter
and none beyond is still refused, as it was: give it a password without one.)
MySQL's values are handed over the same way. SQL Server's
user and password are not in its connection string at all but handed to ADO's
`Open` beside it, and its server and database are in the quotes ADO reads. Up to
6.3.5 the three were written into the string as they were, so DBSetup refused a
single quote or a backslash for PostgreSQL, and for SQL Server a semicolon or a
quote as the first character; it no longer does. It still refuses, by name,
before anything is changed: a control character in any of them; a host or a
user that begins and ends with the same quotation mark, which `hMailServer.ini`,
where both are written as they are, would take off when it is read (the password
is kept there encrypted); and for SQL Server a host with a double quote beside a
single quote or a semicolon, which no connection string ADO reads can carry.

**SQL Server with Windows authentication.** The database step runs inside the
service while it is still `LocalSystem`, so SQL Server sees this machine's
account (`DOMAIN\MACHINE$`, or `NT AUTHORITY\SYSTEM` on the same machine), which
must be allowed to create the database. A new installation then moves the
service to `NT SERVICE\hMailServer`, which on a SQL Server on the same machine
needs a login, a user in that database and `db_owner` of its own. Setup gives it
them before the move - `hMailServer.exe --grant-database-login`, as the
administrator running Setup, which therefore has to be a SQL Server administrator
there (the one who installed SQL Server Express usually is) and able to open the
database ([ServiceAccount.md](ServiceAccount.md), "Its login on SQL Server").
Where it cannot, on a database Setup has just created, the service stays on
LocalSystem and serves mail, the message gives the statements a SQL Server
administrator runs, and Setup exits with 10; once they have run, move the service
from the Control Panel, or run Setup again with `/serviceaccount=virtual`. On a
database that existed (`/createdatabase=0`) the move is made, and the service is
held to opening the database: one that does not is moved back to LocalSystem,
with the same message and exit code. Until 6.3.6 Setup moved the service without
the login, left a server that opened no database and listened on no port, and
exited with 0. To keep the service on LocalSystem from the start, install with
`/serviceaccount=localsystem`. A SQL Server on another machine needs nothing:
there the virtual account is this machine's account, as LocalSystem is.

**By hand.** `DBSetup.exe /unattended`, from an elevated prompt in the `Bin`
folder with the variables set, does the same without the installer. It is a
Windows program with no console of its own, so its lines are seen with
`cmd /c "DBSetup.exe /unattended > dbsetup.txt 2>&1"`. Without `/unattended`,
DBSetup is the wizard, whatever the environment holds. After a new installation
whose database step did not complete, either of them then finishes it as Setup
would have - the secure defaults, the profile chosen, the stored-secrets key, and
the service's account with its login on SQL Server - each step said as it goes;
for that it has to run elevated: `/unattended` without elevation refuses with exit
code 2 before it changes anything, and the wizard asks Windows for elevation
itself.

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
`%hMailServerLibs%\openssl-4.0.3-arm64`, `boost_1_92_0-arm64` and
`postgresql-18.6-arm64`, with the same layout inside as the x64 trees:

    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-openssl.ps1 -Version 4.0.3 -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-boost.ps1 -Version 1.92.0 -Platform ARM64
    powershell -NoProfile -ExecutionPolicy Bypass -File libraries\build-pgsql.ps1 -Version 18.6 -OpenSSLVersion 4.0.3 -Platform ARM64

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
  found this from the compiler's warning CS8012. The same warning came back on 28
  September 2026 for an ARM64 publish made after an x64 one in the same
  checkout: MSBuild found the x64 wrapper in each tool's own `publish` folder,
  which it searches before a reference's hint path, and the add-ons' `Addons`
  folders would have carried it. The `publish` and `publish-arm64` folders are
  now kept out of the projects' items (`Tools\Directory.Build.props`), and the
  wrapper each publish carries is the one its platform names, whatever was
  published before.
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
- The ARM64 installer's SBOM is made from `build/native-dependencies.ps1
  -Platform ARM64`'s component list, which the `linux-sbom` job writes as
  `native-dependencies-arm64.json`. The list takes each linked library's version
  from the ARM64 include paths of `hMailServer.vcxproj`, and the ARM64 .NET
  runtime, C++ runtime and 7-Zip from `third-party-binaries.json`. On the machine
  that built the installer, `-LibrariesRoot %hMailServerLibs%` also holds each
  library to the tree its path names, which records its own version (OpenSSL's
  `VERSION.dat`, Boost's `boost\version.hpp`, PostgreSQL's `meson.build`); the
  list is the same with it as without it.

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
   PostgreSQL, silently ([A new installation without a window](#a-new-installation-without-a-window)),
   and once more against SQL Server through the ARM64 OLE DB Driver 19. The
   smoke test does this, run on that machine as `installer-smoke.ps1 -Installer
   <the ARM64 installer> -DatabaseSettings <file> -AllowUnsigned` (below). Open
   the Control Panel and send and receive a message.
3. Run the regression suite against that server on PostgreSQL or SQL Server.
   It cannot use the built-in database, so the fixtures that assume it are
   skipped or pointed at the external database, as on the second bench.
4. Take a backup and restore it (the ARM64 7-Zip).
5. Install it over an x64 installation that uses SQL Server Compact, and see it
   refused with nothing changed.

Until then this page, `section_files_arm64.iss` and the libraries' ARM64 paths
are a build that has never run.

What is proven, and what needs ARM64 hardware
---------------------------------------------

**Shown on x64, on the maintainer's laptop, 28 September 2026.** No installer
was run there: the laptop runs a hMailServer service somebody depends on.

- `DBSetup.exe /unattended`, x64 build, run as the installer runs it (through
  `cmd.exe`, its lines captured): 20 sets of settings it refuses, each with
  exit code 2 and its reason, before it contacts the server, with no window,
  and never a password in its output; and 6 that pass every check, stopped
  only by a service name that does not exist, the last check before the
  server. The setting reader, run in a process of its own over 22 more cases:
  the password file with a byte order mark and CRLF, the defaults, the aliases,
  and the bounds of the names and ports.
- Both installers compile with ISCC from this tree's scripts and tools, the
  server not being rebuilt for this change. The x64 one used the server of the
  laptop's own x64 build (27 September, from master). The ARM64 one used the
  ARM64 libraries of 26 September, and four stand-in files for the ARM64
  server, whose build was not on the laptop; what it made was deleted, not run. `check-pe-machine.ps1` over what
  each installer packs: x64, 85 images, all right; ARM64, 72 images, 71 right
  and the ARM64EC `vcruntime140_1.dll` allowed, as the CI job allows it.
- The ARM64 publish's COM wrapper, made after an x64 publish from clean
  `publish` folders: five x64 wrappers before the change to
  `Tools\Directory.Build.props`, none after.
- `native-dependencies.ps1 -Platform ARM64 -LibrariesRoot C:\Dev\hMailLibs`:
  OpenSSL 4.0.2, Boost 1.92.0 and libpq 18.3, each what its `-arm64` tree
  records. The x64, ARM64 and `-LinuxOnly` lists are byte for byte what they
  were.
- Re-checked the same evening: 38 cases through `cmd.exe`, as above. Two gaps
  were found and closed: a password file in the ANSI code page was read with
  U+FFFD in place of each letter outside ASCII (it is refused now), and a SQL
  Server password, user or host beginning with a quote passed DBSetup's checks
  although OLE DB cannot parse it (ADODB with `sqloledb` answered "Format of the
  initialization string does not conform to the OLE DB specification" before
  any connection; refused now). Both installers compiled again, and the ARM64
  publish of DBSetup, made beside an x64 `publish` folder, took the ARM64
  wrapper.

**Not shown on the laptop: DBSetup `/unattended` against a server.** DBSetup
works through the server's COM object, and the only one registered on the
laptop is its running service. `CreateExternalDatabase` would have rewritten
that service's `[Database]` section. So the path from the installer through
DBSetup to a database at the build's schema is the smoke test's to show, on the
smoke VM, with the x64 installer:

    bash build/ci/installer-smoke-run.sh --allow-unsigned --database database.env \
       --database-password database-password.txt hMailServer-<version>-<commit>-x64.exe

`database.env` holds `HM_DB_TYPE=PostgreSQL`, `HM_DB_HOST=` a PostgreSQL server
the VM reaches, `HM_DB_USER=` a role that may create databases, `HM_DB_NAME=` a
database that does not exist yet, and
`HM_DB_PASSWORD_FILE=C:\smoke\database-password.txt`. Run it again with
`--database-in-environment`, the installer's other route. Each run checks the
install, DBSetup's exit 0 in Inno's log, `[Database]` naming the database, and
its schema over COM, then uninstalls. The database it made is the run's to drop.

**Needs ARM64 hardware:** that any of the ARM64 build runs at all - the server,
DBSetup, DBUpdater, the Control Panel, the ARM64 .NET runtime's installer - and
the list above under "What a first run on ARM64 hardware has to show".
