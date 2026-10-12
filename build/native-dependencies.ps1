# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Emits, as JSON, every component the SBOM has to name that Syft cannot find in the
# tree, for build/merge-native-dependencies-into-sbom.ps1 to add to it.
#
# Why this has to exist. The linux-sbom job runs Syft over the repository, and Syft can
# only report what it can see. Boost, OpenSSL and libpq are not in the repository - they
# are built separately and referenced through $(hMailServerLibs) - so a directory scan
# finds no trace of them. The result was a shipped SBOM listing 54 packages, every one of
# them .NET, for a product whose TLS is OpenSSL and whose entire socket layer is Boost
# Asio. Anyone using that SBOM the way an SBOM is meant to be used - checking a release
# against a CVE feed - would have concluded this server does not link OpenSSL at all.
# An SBOM that is silently missing the parts most likely to have a CVE is worse than no
# SBOM, because it answers the question wrongly instead of not answering it.
#
# Four kinds of component, each read from the file the build itself reads, so that none
# of them is a second copy of the truth that can rot:
#
#   linked     OpenSSL, Boost and libpq, from the include paths in hMailServer.vcxproj,
#              which is what the compiler is actually given, so they cannot be stale
#              while the build works - if the version in the path is wrong, the build
#              fails rather than the SBOM.
#   vendored   Source in the tree that Syft sees as C files with no package manifest to
#              name them by: QuickJS, the JavaScript engine the event scripts run in on
#              the Linux builds (built by CMake alone; the release has no MSVC support),
#              with its version from the VERSION file its release ships, which is where
#              the CMake build reads CONFIG_VERSION from; and zlib, compiled into
#              hMailServer.exe for deflate, with its version from zlib.h's ZLIB_VERSION.
#   shipped    What the Windows installer carries and git does not: every entry of
#              hmailserver/docs/third-party-binaries.json whose kind carries a pin -
#              fetched (a SHA-256), gathered (a version) and downloaded (a SHA-512) -
#              with the version the manifest records and the hash the build checks.
#              That is 7-Zip, the MariaDB client, SQL Server Compact, the MSVC runtime
#              and the .NET Desktop Runtime. The two kinds left out are left out on
#              purpose: "system" is Windows' own ADO type library, compiled against as
#              the Windows SDK's headers are and never redistributed, and "generated"
#              is this project's own COM interop assembly.
#   system     The Linux packages' libraries: one component for each library
#              hmailserver/source/Server/CMakeLists.txt finds (find_package and
#              find_library), and the C++ runtime. They are the distribution's - Boost
#              the build image's, linked statically - so no version is claimed for
#              them: the build log records the one each package was built against.
#
# Every table below fails loudly on what it does not know: a dependency whose version
# cannot be read, a manifest component or a CMake library with no entry here. The day
# someone adds an input and forgets this file, the SBOM job fails rather than shipping a
# bill of materials that is silently one short. build/check-sbom-inputs.py holds the
# same rule over the tree without running this, and holds a finished SBOM to it.
#
# The licences are declared here because a path cannot carry one. They are SPDX
# identifiers, or NOASSERTION with the licence's name beside it where SPDX has none.
#
# Each component also says which compiled assets carry it ("targets": windows, linux)
# and, for a Linux library, whether the packages link it statically ("linkage"), so
# that build/asset-sbom.py can give every release asset an SBOM of its own that lists
# what that asset holds and nothing else. -Platform ARM64 gives the ARM64 installer's
# list: the linked libraries from the ARM64 include paths, and the shipped files
# third-party-binaries.json tags arm64 in place of the x64 ones. -LibrariesRoot, on the
# machine that built the installer, holds each linked library's version to the built
# tree its include path names, which records its own.

[CmdletBinding()]
param(
   # Where to write the JSON. Defaults to stdout, which is what CI uses.
   [string] $OutputFile,
   # The Windows installer the linked and shipped components are those of.
   [ValidateSet('x64', 'ARM64')] [string] $Platform = 'x64',
   # The Linux packages' components alone: QuickJS and the system libraries
   # CMakeLists.txt finds, which is all a .deb's or an .rpm's SBOM takes from this list
   # (build/asset-sbom.py). The Windows build's inputs - hMailServer.vcxproj, the
   # vendored zlib.h, third-party-binaries.json - are not read, so it runs in the source
   # tarball, which carries neither of the first two (build/source-exclusions.txt):
   # build/ci/linux-build.sh passes it when it builds from that tarball.
   [switch] $LinuxOnly,
   # The tree to read, when it is not the one this script is in: a release's tree,
   # checked out elsewhere, described by a newer copy of this script.
   [string] $RepoRoot,
   # The libraries the Windows build linked (%hMailServerLibs%, which build/build.ps1
   # and the installer scripts read): each linked library's version, taken from the
   # include path as always, is then also read from the built tree that path names
   # (openssl-4.0.3-arm64, boost_1_92_0-arm64, postgresql-18.6-arm64 for -Platform
   # ARM64), from the file the library's own build records its version in, and a tree
   # that is missing or says another version fails the list. The list itself is the
   # same with it as without it: this only proves that it names what the machine
   # that runs it built. For the build machine - bench150's windows-packages jobs,
   # the laptop - not the linux-sbom job, which has no libraries.
   [string] $LibrariesRoot
)

$ErrorActionPreference = 'Stop'

$repoRoot = if ($RepoRoot) { (Resolve-Path -LiteralPath $RepoRoot).Path } else { Split-Path -Parent $PSScriptRoot }

# Built up a segment at a time rather than as one backslash-separated string, because this
# runs on linix in the linux-sbom job as well as on Windows, and a literal
# 'a\b\c' is one filename containing backslashes there rather than a path.
function Join-RepoPath([string[]] $Segments) {
   $path = $repoRoot
   foreach ($segment in $Segments) { $path = Join-Path $path $segment }
   return $path
}

$projectFile = Join-RepoPath @('hmailserver', 'source', 'Server', 'hMailServer', 'hMailServer.vcxproj')
$manifestFile = Join-RepoPath @('hmailserver', 'docs', 'third-party-binaries.json')
$cmakeFile = Join-RepoPath @('hmailserver', 'source', 'Server', 'CMakeLists.txt')

if ($LinuxOnly -and $Platform -ne 'x64') { throw '-LinuxOnly is the Linux packages'' list, which has no -Platform.' }
if ($LinuxOnly -and $LibrariesRoot) { throw '-LinuxOnly reads no Windows library; -LibrariesRoot is for the Windows lists.' }
if ($LibrariesRoot -and -not (Test-Path -LiteralPath $LibrariesRoot -PathType Container)) { throw "-LibrariesRoot $LibrariesRoot is not a directory." }
$inputs = if ($LinuxOnly) { @($cmakeFile) } else { @($projectFile, $manifestFile, $cmakeFile) }
foreach ($required in $inputs) {
   if (-not (Test-Path -LiteralPath $required)) { throw "Cannot find $required, which the SBOM's component list is read from." }
}

$project = if ($LinuxOnly) { '' } else { Get-Content -LiteralPath $projectFile -Raw }

# ---------------------------------------------------------------------------------
# Linked and vendored: each entry, the regex that finds the version in an include or
# library path (or the file and the regex that read it), and the metadata a path cannot
# carry. Anchored on the directory-name conventions the upstream projects themselves
# use, so an upgrade that follows those conventions is picked up with no change here.
# ---------------------------------------------------------------------------------
$dependencies = @(
   @{
      Name    = 'openssl'
      Pattern = 'openssl-(\d+\.\d+\.\d+[a-z]?)'
      Purl    = 'pkg:generic/openssl@{0}'
      Cpe     = 'cpe:2.3:a:openssl:openssl:{0}:*:*:*:*:*:*:*'
      License = 'Apache-2.0'
      Website = 'https://www.openssl.org/'
      Why     = 'TLS for SMTP, POP3, IMAP and the REST API, plus DKIM signing and hashing'
   },
   @{
      Name    = 'boost'
      # boost_1_92_0 -> 1.92.0
      Pattern = 'boost_(\d+)_(\d+)_(\d+)'
      Purl    = 'pkg:generic/boost@{0}'
      Cpe     = 'cpe:2.3:a:boost:boost:{0}:*:*:*:*:*:*:*'
      License = 'BSL-1.0'
      Website = 'https://www.boost.org/'
      Why     = 'Asio for all socket I/O, plus threading, filesystem and string algorithms'
   },
   @{
      Name    = 'postgresql'
      Pattern = 'postgresql-(\d+\.\d+)'
      Purl    = 'pkg:generic/postgresql@{0}'
      Cpe     = 'cpe:2.3:a:postgresql:postgresql:{0}:*:*:*:*:*:*:*'
      License = 'PostgreSQL'
      Website = 'https://www.postgresql.org/'
      Why     = 'libpq, the client library for the PostgreSQL database backend'
   },
   @{
      Name        = 'quickjs'
      # Read from this file rather than matched in the project file; see the header.
      VersionFile = @('hmailserver', 'source', 'Server', 'quickjs', 'VERSION')
      Vendored    = 'hmailserver/source/Server/quickjs'
      # The Linux packages' binary alone: hMailServer.exe has no QuickJS.
      Targets     = 'linux'
      Purl        = 'pkg:generic/quickjs@{0}'
      Cpe         = 'cpe:2.3:a:quickjs_project:quickjs:{0}:*:*:*:*:*:*:*'
      License     = 'MIT'
      Website     = 'https://bellard.org/quickjs/'
      Why         = 'the JavaScript engine the event scripts run in on the Linux builds (hMailServer.exe does not contain it: its event scripts are VBScript and JScript)'
   },
   @{
      Name           = 'zlib'
      # zlib.h's own #define, which is the version the compiled library reports.
      VersionFile    = @('hmailserver', 'source', 'Server', 'zlib', 'zlib.h')
      VersionPattern = '#define ZLIB_VERSION "([^"]+)"'
      Vendored       = 'hmailserver/source/Server/zlib'
      # hMailServer.exe alone: the Linux packages link the distribution's zlib.
      Targets        = 'windows'
      Purl           = 'pkg:generic/zlib@{0}'
      Cpe            = 'cpe:2.3:a:zlib:zlib:{0}:*:*:*:*:*:*:*'
      License        = 'Zlib'
      Website        = 'https://zlib.net/'
      Why            = 'deflate for IMAP COMPRESS=DEFLATE, the compressed message store and compressed HTTP responses, compiled into hMailServer.exe (the Linux builds link the distribution''s zlib instead)'
   }
)

# ---------------------------------------------------------------------------------
# Shipped: the metadata for each component of third-party-binaries.json whose kind
# carries a pin, keyed by the manifest's "component". The version and the hashes come
# from the manifest. A CPE is given only where NVD's name for the product is known.
# ---------------------------------------------------------------------------------
$pinnedDispositions = @('fetched', 'gathered', 'downloaded')

$installerComponents = @{
   '7-Zip standalone console (7za.exe)' = @{
      Id      = '7-zip'
      Purl    = 'pkg:generic/7-zip@{0}'
      Cpe     = 'cpe:2.3:a:7-zip:7-zip:{0}:*:*:*:*:*:*:*'
      License = 'LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause'
      Website = 'https://www.7-zip.org/'
      Why     = 'the standalone console, 7za.exe, which the backup writes and reads its archives with'
   }
   'MariaDB Connector/C' = @{
      Id      = 'mariadb-connector-c'
      Purl    = 'pkg:generic/mariadb-connector-c@{0}'
      Cpe     = 'cpe:2.3:a:mariadb:connector\/c:{0}:*:*:*:*:*:*:*'
      License = 'LGPL-2.1-or-later'
      Website = 'https://mariadb.com/docs/connectors/mariadb-connector-c/'
      Why     = 'the MySQL and MariaDB client, libmysql.dll and its seven authentication plugins, loaded at run time, built from MariaDB''s source by libraries/build-mariadb-connector-c.ps1; the hash is that of the archive fetched, and each file''s own is in third-party-binaries.json'
   }
   'Microsoft SQL Server Compact 4.0 SP1 x64 ENU' = @{
      Id          = 'sql-server-compact'
      Purl        = 'pkg:generic/sql-server-compact@4.0-sp1'
      Cpe         = 'cpe:2.3:a:microsoft:sql_server_compact:4.0:sp1:*:*:*:*:*:*'
      License     = 'NOASSERTION'
      LicenseName = 'Microsoft SQL Server Compact Edition EULA'
      Website     = 'https://www.microsoft.com/'
      Why         = 'the runtime of the built-in database backend, SSCERuntime_x64-ENU.msi, which Microsoft no longer publishes'
   }
   'Microsoft Visual C++ Redistributable (MSVC v145 runtime)' = @{
      Id          = 'msvc-runtime'
      Purl        = 'pkg:generic/microsoft-visual-cpp-redistributable@{0}'
      Cpe         = ''
      License     = 'NOASSERTION'
      LicenseName = 'Microsoft Visual Studio Distributable Code terms'
      Website     = 'https://visualstudio.microsoft.com/'
      Why         = 'the C and C++ runtime DLLs installed beside the server, gathered from the build machine''s Visual Studio and checked by version against the toolset that built it'
   }
   'Microsoft .NET Windows Desktop Runtime (x64 installer)' = @{
      Id          = 'dotnet-windowsdesktop-runtime'
      Purl        = 'pkg:generic/dotnet-windowsdesktop-runtime@{0}'
      Cpe         = 'cpe:2.3:a:microsoft:.net:{0}:*:*:*:*:*:*:*'
      License     = 'NOASSERTION'
      LicenseName = 'Microsoft .NET runtime licence terms'
      Website     = 'https://dotnet.microsoft.com/'
      Why         = 'the .NET runtime and Windows Desktop runtime the Control Panel and the database tools run on, installed by the installer where no .NET 10 is present; pinned to one release by build/get-dotnet-runtime.ps1'
   }
   'Microsoft .NET Windows Desktop Runtime (ARM64 installer)' = @{
      Id          = 'dotnet-windowsdesktop-runtime-arm64'
      Purl        = 'pkg:generic/dotnet-windowsdesktop-runtime@{0}?arch=arm64'
      Cpe         = 'cpe:2.3:a:microsoft:.net:{0}:*:*:*:*:*:*:*'
      License     = 'NOASSERTION'
      LicenseName = 'Microsoft .NET runtime licence terms'
      Website     = 'https://dotnet.microsoft.com/'
      Why         = 'the same runtime for Windows on ARM, carried by the ARM64 installer (hmailserver/docs/WindowsOnArm.md); pinned to the same release by build/get-dotnet-runtime.ps1 -Platform ARM64'
   }
}

# ---------------------------------------------------------------------------------
# System (Linux): each name CMakeLists.txt finds, and the component it is. Threads and
# Iconv are the C library's on glibc; libldap and liblber are both OpenLDAP's.
# ---------------------------------------------------------------------------------
$linuxLibraries = @{
   'Threads'    = 'glibc'
   'Iconv'      = 'glibc'
   'OpenSSL'    = 'openssl'
   'ZLIB'       = 'zlib'
   'Boost'      = 'boost'
   'PostgreSQL' = 'libpq'
   'ldap'       = 'openldap'
   'lber'       = 'openldap'
}

# The C++ runtime is linked by every C++ program and found by no find_package, so it is
# named whatever the list above finds.
$linuxAlways = @('libstdcxx')

$linuxComponents = @{
   'glibc' = @{
      Name = 'glibc'; Purl = 'pkg:generic/glibc'; License = 'LGPL-2.1-or-later'; Website = 'https://www.gnu.org/software/libc/'
      Why  = 'the C library: threads, iconv (every conversion from another character set) and dlopen, through which the MySQL client is loaded'
   }
   'libstdcxx' = @{
      Name = 'libstdc++'; Purl = 'pkg:generic/libstdc%2B%2B'; License = 'GPL-3.0-or-later WITH GCC-exception-3.1'; Website = 'https://gcc.gnu.org/onlinedocs/libstdc++/'
      Why  = 'the C++ standard library, with clang as with GCC'
   }
   'openssl' = @{
      Name = 'openssl'; Purl = 'pkg:generic/openssl'; License = 'Apache-2.0'; Website = 'https://www.openssl.org/'
      Why  = 'libssl and libcrypto, 3.0 or later: TLS and every hash and signature'
   }
   'zlib' = @{
      Name = 'zlib'; Purl = 'pkg:generic/zlib'; License = 'Zlib'; Website = 'https://zlib.net/'
      Why  = 'deflate (the vendored zlib is compiled into hMailServer.exe only)'
   }
   'boost' = @{
      Name = 'boost'; Purl = 'pkg:generic/boost'; License = 'BSL-1.0'; Website = 'https://www.boost.org/'
      Why  = 'the build image''s Boost, 1.83 or later, linked statically: thread, chrono and filesystem, and the header-only rest'
      # In the binary itself; every other library here is the distribution's, found at run time.
      Linkage = 'static'
   }
   'libpq' = @{
      Name = 'libpq'; Purl = 'pkg:generic/libpq'; License = 'PostgreSQL'; Website = 'https://www.postgresql.org/'
      Why  = 'the PostgreSQL client library'
   }
   'openldap' = @{
      Name = 'openldap'; Purl = 'pkg:generic/openldap'; License = 'OLDAP-2.8'; Website = 'https://www.openldap.org/'
      Why  = 'libldap and liblber, the directory client that Active Directory and LDAP authentication use'
   }
}

$found = @()
$missing = @()
$treeProblems = @()

# -LibrariesRoot: the version a linked library's built tree records of itself, from the
# file its own build writes or ships - OpenSSL's VERSION.dat, Boost's boost/version.hpp
# (BOOST_VERSION, 109200 for 1.92.0), PostgreSQL's meson.build - in the form the include
# path's version takes. '' when the file is not there or does not say.
function Get-TreeVersion([string] $Name, [string] $Tree) {
   switch ($Name) {
      'openssl' {
         $file = Join-Path $Tree 'VERSION.dat'
         if (-not (Test-Path -LiteralPath $file)) { return '' }
         $text = Get-Content -LiteralPath $file -Raw
         # Collected with +=, not as $parts = foreach (...): a return inside a loop
         # whose output is being assigned sends the fields already read to the
         # function's output with the '', and a VERSION.dat without PATCH was
         # reported as recording "4 0 " rather than no version.
         $parts = @()
         foreach ($field in @('MAJOR', 'MINOR', 'PATCH')) {
            $m = [regex]::Match($text, "(?m)^$field=(\d+)\s*$")
            if (-not $m.Success) { return '' }
            $parts += $m.Groups[1].Value
         }
         return ($parts -join '.')
      }
      'boost' {
         $file = Join-Path (Join-Path $Tree 'boost') 'version.hpp'
         if (-not (Test-Path -LiteralPath $file)) { return '' }
         $m = [regex]::Match((Get-Content -LiteralPath $file -Raw), '(?m)^#define BOOST_VERSION (\d+)\s*$')
         if (-not $m.Success) { return '' }
         $number = [int] $m.Groups[1].Value
         return ('{0}.{1}.{2}' -f [math]::Floor($number / 100000), ([math]::Floor($number / 100) % 1000), ($number % 100))
      }
      'postgresql' {
         $file = Join-Path $Tree 'meson.build'
         if (-not (Test-Path -LiteralPath $file)) { return '' }
         $m = [regex]::Match((Get-Content -LiteralPath $file -Raw), "(?m)^\s*version:\s*'(\d+\.\d+)")
         if (-not $m.Success) { return '' }
         return $m.Groups[1].Value
      }
   }
   return ''
}

function New-Component([hashtable] $fields) {
   $component = [ordered]@{
      id          = $fields.id
      name        = $fields.name
      version     = [string] $fields.version
      kind        = $fields.kind
      purl        = $fields.purl
      cpe         = [string] $fields.cpe
      license     = $fields.license
      licenseName = [string] $fields.licenseName
      website     = $fields.website
      why         = $fields.why
      source      = $fields.source
      vendored    = $fields.vendored
      hashes      = @($fields.hashes | Where-Object { $_ })
      # The compiled assets that carry it (windows: the installer of -Platform; linux:
      # the .deb and the .rpm), and how a Linux library is linked (static or dynamic).
      targets     = @($fields.targets)
      linkage     = [string] $fields.linkage
      files       = @($fields.files | Where-Object { $_ })
   }
   return [pscustomobject] $component
}

# ------------------------------------------------------------------ linked, vendored
foreach ($dependency in $dependencies) {
   # -LinuxOnly: an entry no Linux package carries is not read at all.
   if ($LinuxOnly -and @($dependency.Targets) -notcontains 'linux') { continue }
   if ($dependency.VersionFile) {
      $versionFile = Join-RepoPath $dependency.VersionFile
      $text = if (Test-Path -LiteralPath $versionFile) { Get-Content -LiteralPath $versionFile -Raw } else { '' }
      $version = ''
      if ($text) {
         if ($dependency.VersionPattern) {
            $match = [regex]::Match($text, $dependency.VersionPattern)
            if ($match.Success) { $version = $match.Groups[1].Value }
         }
         else {
            $version = $text.Trim()
         }
      }

      if (-not $version) {
         $missing += $dependency.Name
         continue
      }

      $source = ($dependency.VersionFile -join '/')
   }
   else {
      # The ARM64 configuration's own paths, which carry the suffix -arm64: the x64
      # ones are not what the ARM64 installer is linked against.
      $pattern = if ($Platform -eq 'ARM64') { $dependency.Pattern + '-arm64' } else { $dependency.Pattern }
      $match = [regex]::Match($project, $pattern)

      if (-not $match.Success) {
         $missing += $dependency.Name
         continue
      }

      # Boost's directory convention is underscores; everything else already reads as a
      # version. Joining the numeric groups keeps this general rather than special-casing
      # the name.
      $groups = @()
      for ($i = 1; $i -lt $match.Groups.Count; $i++) { $groups += $match.Groups[$i].Value }
      $version = $groups -join '.'

      $source = 'hMailServer.vcxproj include path'

      # The tree the path names, when the build's libraries were given: it must be
      # there and record the same version (Get-TreeVersion).
      if ($LibrariesRoot) {
         $tree = Join-Path $LibrariesRoot $match.Value
         $treeVersion = if (Test-Path -LiteralPath $tree -PathType Container) { Get-TreeVersion $dependency.Name $tree } else { $null }
         if ($null -eq $treeVersion) { $treeProblems += "$($dependency.Name): $tree, which the include path names, is not there" }
         elseif ($treeVersion -eq '') { $treeProblems += "$($dependency.Name): $tree records no version where its build writes one" }
         elseif ($treeVersion -ne $version) { $treeProblems += "$($dependency.Name): $tree records $treeVersion, and the include path names $version" }
         else { Write-Verbose "$($dependency.Name) $version is what $tree records" }
      }
   }

   $found += New-Component @{
      id       = $dependency.Name
      name     = $dependency.Name
      version  = $version
      kind     = $(if ($dependency.Vendored) { 'vendored' } else { 'linked' })
      purl     = $dependency.Purl -f $version
      cpe      = $dependency.Cpe -f $version
      license  = $dependency.License
      website  = $dependency.Website
      why      = $dependency.Why
      source   = $source
      vendored = $dependency.Vendored
      targets  = $(if ($dependency.Targets) { @($dependency.Targets) } else { @('windows') })
   }
}

if ($missing.Count -gt 0) {
   # A hard failure, not a warning. This script exists to stop the SBOM omitting things
   # silently, so it must not itself omit things silently: if a dependency's path
   # convention changed, that is a two-minute fix to a regex above, and it should block
   # the release rather than quietly shrink the SBOM back to being wrong. The same holds
   # for a vendored dependency whose VERSION file has gone.
   # The sentence is joined before -f is applied: -f binds more tightly than +, so
   # applied to the last piece alone it left the {0} in the first one unfilled.
   throw (("Could not determine the version of: {0}. Either the include paths in " +
           "hMailServer.vcxproj no longer match the expected pattern, or a vendored " +
           "dependency's version file is missing or no longer matches. Fix the pattern or the path in " +
           "this script - do not remove the dependency, because it is still linked.") -f ($missing -join ', '))
}

if ($treeProblems.Count -gt 0) {
   throw ("The libraries under -LibrariesRoot are not the ones the list names: " + ($treeProblems -join '; ') +
          ". Build them with libraries\build-*.ps1 at the versions hMailServer.vcxproj names, or point -LibrariesRoot at the folder the build used.")
}

# ------------------------------------------------------------------------- shipped
# -LinuxOnly: what the Windows installer carries is no Linux package's.
$manifest = if ($LinuxOnly) { [pscustomobject]@{ build_inputs = @(); artifacts = @() } } else { Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json }
$buildInputs = @{}
foreach ($buildInput in @($manifest.build_inputs)) { $buildInputs[('{0}/{1}' -f $buildInput.release, $buildInput.asset)] = ([string] $buildInput.sha256).ToLowerInvariant() }

$byComponent = [ordered]@{}
foreach ($artifact in @($manifest.artifacts)) {
   if ($pinnedDispositions -notcontains $artifact.disposition) { continue }
   # An artifact tagged with a platform (arm64) ships in the ARM64 installer only,
   # and one with none in the x64 installer only: the ARM64 installer carries its
   # own 7-Zip, runtime and .NET, and no MySQL client or SQL Server Compact
   # (hmailserver/installation/section_files_arm64.iss).
   $wanted = if ($Platform -eq 'ARM64') { 'arm64' } else { '' }
   if ([string] $artifact.platform -ne $wanted) { continue }
   $key = '{0}|{1}' -f $artifact.component, $artifact.version
   if (-not $byComponent.Contains($key)) { $byComponent[$key] = @() }
   $byComponent[$key] += $artifact
}

$unknown = @()
foreach ($key in $byComponent.Keys) {
   $artifacts = @($byComponent[$key])
   $first = $artifacts[0]
   $meta = $installerComponents[[string] $first.component]
   if (-not $meta) { $unknown += ('{0} {1}' -f $first.component, $first.version); continue }

   $hashes = @()
   $pins = @()
   # Each file the installer packs, by its SHA-256 as the manifest pins it (get-installer-binaries.ps1
   # checks every one before the installer is built), named by its path under
   # hmailserver/installation/ (ARM64/ left out for the ARM64 installer's). The copies under
   # libraries/ are what the build links against, not what is packed.
   $files = @()
   foreach ($artifact in $artifacts) {
      $path = [string] $artifact.path
      if ($path -like 'hmailserver/installation/*' -and $artifact.sha256) {
         $packed = $path.Substring('hmailserver/installation/'.Length)
         if ($Platform -eq 'ARM64' -and $packed -like 'ARM64/*') { $packed = $packed.Substring('ARM64/'.Length) }
         $files += [pscustomobject][ordered]@{ path = $packed; sha256 = ([string] $artifact.sha256).ToLowerInvariant() }
      }
   }
   foreach ($artifact in $artifacts) {
      switch ($artifact.disposition) {
         'fetched' {
            $asset = ([string] $artifact.source -split '#', 2)[0]
            if (-not $buildInputs.ContainsKey($asset)) { throw "third-party-binaries.json has no build_inputs hash for $asset, which $($artifact.path) is fetched from." }
            $hashes += [pscustomobject][ordered]@{ algorithm = 'SHA-256'; value = $buildInputs[$asset] }
            $pins += "fetched from $asset"
         }
         'downloaded' {
            $hashes += [pscustomobject][ordered]@{ algorithm = 'SHA-512'; value = ([string] $artifact.sha512).ToLowerInvariant() }
            if ($artifact.sha256) { $hashes += [pscustomobject][ordered]@{ algorithm = 'SHA-256'; value = ([string] $artifact.sha256).ToLowerInvariant() } }
            $pins += "downloaded from $($artifact.source)"
         }
         'gathered' {
            $pins += 'gathered from the build machine''s Visual Studio, checked by version'
         }
      }
   }
   # One entry per hash: the MariaDB client's sixteen files come from one archive.
   $unique = @()
   foreach ($hash in $hashes) {
      if (-not ($unique | Where-Object { $_.algorithm -eq $hash.algorithm -and $_.value -eq $hash.value })) { $unique += $hash }
   }

   $version = [string] $first.version
   $escaped = [uri]::EscapeDataString($version)
   $found += New-Component @{
      id          = $meta.Id
      name        = $meta.Id
      version     = $version
      kind        = 'shipped'
      purl        = $meta.Purl -f $escaped
      cpe         = $(if ($meta.Cpe) { $meta.Cpe -f $version } else { '' })
      license     = $meta.License
      licenseName = $meta.LicenseName
      website     = $meta.Website
      why         = $meta.Why
      source      = ('hmailserver/docs/third-party-binaries.json ({0} {1}: {2})' -f $artifacts.Count, $(if ($artifacts.Count -eq 1) { 'file' } else { 'files' }), (($pins | Select-Object -Unique) -join '; '))
      hashes      = $unique
      targets     = @('windows')
      files       = @($files)
   }
}

if ($unknown.Count -gt 0) {
   throw ("third-party-binaries.json pins {0}, which this script has no SBOM entry for. Add it to `$installerComponents here, with its purl, CPE and licence - do not leave it out, because the installer carries it." -f ($unknown -join ', '))
}

# -------------------------------------------------------------------- system (Linux)
$cmake = Get-Content -LiteralPath $cmakeFile -Raw
$names = @()
foreach ($m in [regex]::Matches($cmake, '(?m)^\s*find_package\(\s*(\w+)')) { $names += $m.Groups[1].Value }
foreach ($m in [regex]::Matches($cmake, '(?m)^\s*find_library\(\s*\w+\s+NAMES\s+(\w+)')) { $names += $m.Groups[1].Value }

$unmapped = @($names | Where-Object { -not $linuxLibraries.ContainsKey($_) })
if ($unmapped.Count -gt 0) {
   throw ("CMakeLists.txt finds {0}, which this script has no SBOM entry for. Add it to `$linuxLibraries here - do not leave it out, because the Linux packages link it." -f ($unmapped -join ', '))
}

$linuxIds = @()
foreach ($name in $names) { $linuxIds += $linuxLibraries[$name] }
$linuxIds = @($linuxIds + $linuxAlways | Select-Object -Unique)

foreach ($id in $linuxIds) {
   $meta = $linuxComponents[$id]
   if (-not $meta) { throw "No component is described for $id; add it to `$linuxComponents." }
   $findNames = @($names | Where-Object { $linuxLibraries[$_] -eq $id })
   $found += New-Component @{
      id      = 'linux-' + $id
      name    = $meta.Name
      version = ''
      kind    = 'system'
      purl    = $meta.Purl
      cpe     = ''
      license = $meta.License
      website = $meta.Website
      why     = $meta.Why
      source  = $(if ($findNames.Count) { 'hmailserver/source/Server/CMakeLists.txt finds it (' + ($findNames -join ', ') + ')' } else { 'every C++ program links it' })
      targets = @('linux')
      linkage = $(if ($meta.Linkage) { $meta.Linkage } else { 'dynamic' })
   }
}

# PowerShell 5.1 and 7 both write this with the same content; the arrays stay arrays
# because the list is written whole rather than piped.
$json = ConvertTo-Json -InputObject @($found) -Depth 6

if ($OutputFile) {
   $json | Set-Content -Path $OutputFile -Encoding UTF8
   Write-Host "Wrote $($found.Count) components to $OutputFile"
   foreach ($component in $found) { Write-Host ("  {0,-8} {1} {2}" -f $component.kind, $component.id, $component.version) }
}
else {
   $json
}
