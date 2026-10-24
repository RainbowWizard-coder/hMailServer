The source tarball
==================

**`hmailserver-<version>.tar.gz` is the source the Linux server is built from:
the tree of one commit, less what is not free and what only the Windows build
and its installer use. The distributions build from it, and CI builds and tests
the Linux server from it, so a path the build needs cannot be left out without
a pipeline failing.**

It is made by `build/ci/source-tarball.sh` from the paths
`build/source-exclusions.txt` lists, and checked by
`build/check-source-tarball.py`. The `source-tarball` job
(`.gitlab/ci/source-tarball.yml`) makes it in every pipeline that builds the
Linux server, on linix, and `linux-build` and `linux-suite` take it from that
job with `HM_SOURCE_TARBALL`. From 6.3.6 a release carries it, with its
SBOM, a line in the sums and a Sigstore bundle like every other release file:
that is the release chain's part (`build/ci/release-chain.py`), which collects
it from the job.

**Where it stands (6 October 2026):** 6.3.6 carries `hmailserver-6.3.6.tar.gz`
with its SBOM, its line in the sums and its Sigstore bundle, and in its tag
pipeline 2914690858 `linux-build` built the server, its testers and both
packages from it alone (3,605 files) and `linux-suite` tested from it: 2,066
passed, 343 skipped and none failed of 2,409. It first ran in a GitLab pipeline
in `batch42`'s. Before that, on 28 September 2026, it was made and checked on
the laptop, and the server built from it alone in WSL Ubuntu 26.04 (see
[What has run](#what-has-run)).

What is in it
-------------

One directory, `hmailserver-<version>/`, holding the commit's tree as a Linux
checkout of it holds it, less every path the list names. The version is
`HMAILSERVER_VERSION` in `hmailserver/source/Server/Common/Application/Version.h`,
the number every Linux package is named by; on a `v*` tag the script refuses a
tag that names another. It carries its own `build/source-exclusions.txt`, so
anyone holding a tarball can see what was left out of it, and its own checker.

The line endings are what `.gitattributes` asks of a checkout: CRLF for most
text files, as on every platform, and LF for what a POSIX machine executes or
parses (`build/ci/*.sh`, `build/*.sh`, the packaging under
`hmailserver/source/Server/platform/packaging/`, `hmctl`, the list itself).
That is what the Linux build has always been built from in CI.

To make one by hand, from any clone:

```
bash build/ci/source-tarball.sh            # HEAD
bash build/ci/source-tarball.sh v6.3.6     # a tag, or any commit
```

It writes `source-tarball/hmailserver-<version>.tar.gz` (or into `HM_OUT_DIR`)
and prints the commit, its time, the number of entries and the SHA-256. It
needs git and gzip, and reads the version, the list and the attributes from the
commit named, never from the working tree.

What is left out, and why
-------------------------

A path is left out when the Linux build, its tester programs, its packages and
the Linux suite never read it, and it is either not free or used only by the
Windows build and its installer. `build/source-exclusions.txt` is the list, one
literal path per line, with the reason for each group; in short:

| Left out | Why |
|---|---|
| `libraries/` | `libraries/mssqlce3.5`: SQL Server Compact's headers - version 4.0's, in a folder named for 3.5 - under Microsoft's SQL Server Compact licence, which is not free. The rest is the Windows build's alone: the scripts that build Boost, OpenSSL and libpq for MSVC, the licences of those builds and SQL Server Compact 3.5's, and `libraries/fat_cow_icons` (2,800 icons, CC BY 3.0 US: the Creative Commons Attribution 3.0 United States licence), which only the Control Panel shows. |
| `hmailserver/docs/Licenses/License - Microsoft SQL Server Compact Edition.rtf` | SQL Server Compact 4.0 SP1's licence terms, under which the Windows build ships it and has those headers. |
| `LICENSES/LicenseRef-Microsoft-SQL-Server-Compact-4.0.txt` | Their plain text, which `REUSE.toml` gives those headers. |
| `hmailserver/source/Server/COM`, `hMailServer`, `hMailServer.Minidump`, `hMailServer.Updater` | The ATL administration API, the Windows service host, Windows Error Reporting and the live update's Windows helper. `CMakeLists.txt` builds none of them. |
| `hmailserver/source/Server/zlib` | zlib as `hMailServer.exe` compiles it. The POSIX build links the distribution's zlib and, since 28 September 2026, includes the distribution's `zlib.h` too (before that it read the vendored header over the distribution's library). |
| `hmailserver/source/Tools/*` but `Cli` | The Control Panel and the database tools, every one a `net10.0-windows` project. `Cli` stays: the packages install its `hmctl`. |
| `hmailserver/source/Addons` | The VBScript add-ons the Windows installer copies (the Linux server runs its event scripts in JavaScript). |
| `hmailserver/tools/*` but `Scripts` and `Anonymizer.sql` | .NET Framework developer tools, a batch file, a Visual Studio 2005 file. |
| `hmailserver/installation/*` but `WebAdmin/` and `Extras/dh2048.pem` | The Inno Setup sources, pictures and licence page, the socket-check DLL's source, the uninstall helper, the winget and Chocolatey templates, 7-Zip's licence. What stays is what `CMakeLists.txt` installs: the Control Deck and its catalogues, and the Diffie-Hellman group. |

Left in on purpose: the whole of `hmailserver/test` (the Linux suite links about
340 of `RegressionTests`' files and reads `SSL examples`, and the fuzz seeds come
from `TestData`; splitting the test tree by platform would break the day a
fixture is linked), `build/` and `fuzz/` (the Linux build, its packages and the
fuzzing job read them, beside Windows scripts that cost nothing), and the
Windows-only sources inside `Common` that `CMakeLists.txt` leaves out by name
(`WINDOWS_ONLY_SOURCES`), which are part of the same directories.

### How the list is held

`build/check-source-tarball.py`, with no argument, is one of the checks the
`checkers` job runs on every push and merge request. It refuses:

- a line that is not a literal path in the list's format, names a path git does
  not track, or sits under another line;
- a line that leaves out something the Linux build reads: a path
  `CMakeLists.txt` names (and anything inside a directory it installs or globs
  whole), a path `build/ci/linux-build.sh` or `linux-suite.sh` names - or the
  scripts of the other Linux jobs: the package builds (`deb-build.sh`,
  `rpm-build.sh`, the AppImage's, `PKGBUILD`), `linux-fuzz.sh`,
  `linux-cross.sh`, `linux-sanitize.sh`, `linux-coverage.sh`, `linux-repro.sh`
  and `source-tarball.sh` itself - a file `LinuxRegressionTests.csproj` links,
  anything in that project's own directory (the SDK compiles all of it) and
  `global.json`, what the build and the suite read at run time from a path they
  build (`RUNTIME_READS` in the checker: `SSL examples`, the settings baseline,
  the OpenPGP fixtures, `Tools/Cli`), or a header a source of the POSIX build
  includes outside `#ifndef HM_PLATFORM_POSIX`, `#ifdef _WIN32`,
  `#ifdef _MSC_VER` and their like. That last rule found the six sources that
  included `Server/zlib/zlib.h` on every platform;
- a file under a tree that belongs to Windows alone (`libraries`, the four
  Windows server directories and `zlib`, `Tools`, `Addons`, `installation`)
  that the list does not leave out and the Linux build does not read: a new
  installer script or Control Panel project is left out the day it is added, or
  the check names the line to add;
- a licence the tarball must carry that is missing or left out (the next
  section).

It cannot know what a script reads that it does not name, or what a test reads
at run time from a path it builds and `RUNTIME_READS` does not list. The
definitive answer is the build: in every pipeline, `linux-build` and
`linux-suite` run from the tarball alone. (A fixture that skips itself when a
file of the tree is missing, as `RestApiAccountImport.cs` does without
`Tools/Cli`, would not fail the suite, which is why such reads are listed.)

What stays that is not the project's own
----------------------------------------

Everything else in the tarball is the project's, under the AGPL-3.0-or-later in
`LICENSE`. These are not, and each is declared here:

| What | Where | Licence | Its notice |
|---|---|---|---|
| QuickJS 2026-06-04, Fabrice Bellard's JavaScript engine, compiled into the Linux server | `hmailserver/source/Server/quickjs` | MIT | `quickjs/LICENSE`, and `hmailserver/docs/Licenses/License - QuickJS.txt`; the packages install it as `/usr/share/hmailserver/licenses/QuickJS.txt` |
| The MIME encoder and decoder (Jeff Lee) | `hmailserver/source/Server/Common/Mime`, and the copies of two of its functions as 6.3.5 had them that a tester keeps, in `hmailserver/source/Server/Common/Util/InputCostTester.cpp` | BSD-style | `hmailserver/docs/Licenses/License - MIME Message encoder analyzer.rtf`, and the notice `InputCostTester.cpp` quotes above the copies |
| `CStdString` (Joseph M. O'Leary) | `hmailserver/source/Server/Common/Util/StdString.h`, and its earlier `FindNoCase` and `ReplaceNoCase`, which the string parser's tests keep, in `hmailserver/source/Server/Common/Util/Parsing/StringParser.cpp` | its own permissive notice | `hmailserver/docs/Licenses/License - String library.rtf` |
| The RFC 7208 test suite, which `spf-tests` runs | `hmailserver/source/Server/SMTP/SPF/Conformance/rfc7208-tests.yml` | BSD-style | `rfc7208-tests.LICENSE` beside it, and `hmailserver/docs/Licenses/License - RFC 7208 test suite.txt` |
| The Public Suffix List's rules, compiled into the server, which DMARC, the SURBL lookups and the MX-record test read | `hmailserver/source/Server/Common/AntiSpam/DMARC/PublicSuffixListData.h` | MPL-2.0 | the MPL's own notice in the file's header |
| Unicode's data for UTS 46, Unicode 18.0.0 - the IDNA mapping table and its conformance tests, and the normalisation, bidi and joining data - compiled into the server as the tables the SURBL test and the sender lists' URL entries read a link's host with | `hmailserver/source/Server/Common/Idna/ucd`, and the tables made of it beside it, `IdnaTables.cpp` and `IdnaTestData.cpp` | Unicode-3.0 | `ucd/unicode.LICENSE`, the generated files' headers, and `hmailserver/docs/Licenses/License - Unicode data.txt`; the packages install `ucd/unicode.LICENSE` as `/usr/share/hmailserver/licenses/Unicode-data.txt` |
| The HTML standard's list of named character references (the WHATWG's `entities.json`), compiled into the server as the table the same two read an HTML part's character references with | `hmailserver/source/Server/Common/AntiSpam/whatwg`, and the table made of it, `Common/AntiSpam/HtmlReferencesData.cpp` | BSD-3-Clause (a portion of the CC-BY-4.0 standard incorporated into source code) | `whatwg/entities.LICENSE`, the generated file's header, and `hmailserver/docs/Licenses/License - HTML named character references.txt`; the packages install `whatwg/entities.LICENSE` as `/usr/share/hmailserver/licenses/HTML-named-character-references.txt` |
| Icons drawn after Feather's (Cole Bemis's icon set): the webmail's sprite, in the page compiled into the server - eleven of its symbols after Feather's `settings`, `edit-3`, `printer`, `inbox`, `file`, `file-text`, `paperclip`, `log-in`, `external-link`, `bell-off` and `maximize-2`, six as Feather's `x`, `plus` and chevrons are, and others near Feather's icons of the same subjects - and the Control Deck's eye | `hmailserver/source/Server/Common/Util/Portal.html` and the page as `PortalPageData.cpp` embeds it, and `hmailserver/installation/WebAdmin/index.html` | MIT, and the AGPL for the rest of each page | `hmailserver/docs/Licenses/License - Feather icons.txt`, which says which icons are drawn after Feather's; the copyright file the `.deb`, the `.rpm` and the Arch package carry, `debian/copyright`, has it in its paragraph for the three; the packages install the notice as `/usr/share/hmailserver/licenses/Feather-icons.txt` |
| The ATL Active Script host (VisionTech Limited, 2001), which only `hMailServer.exe` compiles | `hmailserver/source/Server/Common/Scripting/ScriptSite.h` | its own permissive notice (use, modify and redistribute, the notice neither removed nor changed, an altered version saying so), and the AGPL for hMailServer's changes | the notice, which opens the file; below it the file says it is altered and gives `SPDX-License-Identifier: LicenseRef-ScriptSite AND AGPL-3.0-or-later` (until 6.3.6 the project's header stood above the notice; `OWN_NOTICE_FIRST` in `build/add-license-headers.py` says why it no longer does) |
| commonmark.js 0.31.2 (John MacFarlane), a reference implementation of CommonMark: parts of its block parser - the regular expressions of its `lib/blocks.js` and `lib/inlines.js`, its reading of a link reference definition and the order of its blocks - ported to Python in the check of the release jobs, which reads the project's Markdown documents for the commands they give | `build/check-release-chain.py` | BSD-2-Clause, and the AGPL for the rest of the file | `hmailserver/docs/Licenses/License - commonmark.js.txt`, which says what the port takes, and `LICENSES/BSD-2-Clause.txt`; `debian/copyright` has a paragraph for it and the licence's text |
| The Contributor Covenant, version 2.1 (Coraline Ada Ehmke and its contributors), which the project's code of conduct is adapted from | `CODE_OF_CONDUCT.md` | CC-BY-4.0, and the AGPL for the project's own parts (its community spaces, its routes for reports and its paragraph on reporting to GitLab or GitHub) | the file's Attribution, which names the Covenant and its licence and says what the project changed, and `LICENSES/CC-BY-4.0.txt`; `debian/copyright` has a paragraph for it and the licence's text |
| DKIM test messages, which the Windows suite and the fuzzing seeds read: most appear to be the test corpus of Mail::DKIM (Jason Long, Messiah College), distributed under Perl's terms | `hmailserver/test/TestData/DKIM` | Perl's (GPL-1.0-or-later or Artistic-1.0-Perl), for those | none in the tree |

And test data with no licence stated at all: messages captured from third
parties - the three DKIM verifiers' replies under
`hmailserver/test/TestData/DKIM/Good` and `PermFail` (`*.response.txt`), the
first two of them again as the MIME fuzzers changed them into regression inputs
of theirs (`fuzz/regression/mime_message_fuzzer/heap-buffer-overflow-findstring-boundary-scan`,
Alt-N's, and `fuzz/regression/mime_decode_fuzzer/out-of-memory-mimeunicodeencoder-encodevalue`,
Eland's, into which the fuzzer spliced fragments of the DKIM test corpus's
messages and of one of the original's test messages, whose holders its record
names beside Eland's), and the Secunia page text in
`hmailserver/test/RegressionTests/Resources/SecuniaBody1.txt`, which the suite
embeds; and a certificate authority's certificate, the Google Internet
Authority's of 2009 as Equifax issued it, in `hmailserver/test/SSL examples`
(`google.cer`, and `example+google.txt` after the original's example
certificate), which nothing reads. The messages are kept because the suite and
the fuzzing read them, and the certificates as the original left them; a
distribution whose archive requires a licence for every file (Debian's does)
has to decide whether to take them as they are or leave them out of its own
repack with `Files-Excluded`, which costs the fixtures that read them. The
Debian source package leaves all eight out ([SourcePackages.md](SourcePackages.md)).
`Test 1.eml` beside the replies is not one of them: it is Martin Knafve's own test
message, which he sent from his Outlook address to his own on 1 January 2021 and
added to the original's test data the next day, and it is the original's, under
its licence.
For REUSE (`REUSE.toml`, `LICENSES/`; CONTRIBUTING.md, "Code style and licence
headers") they are `LicenseRef-No-Licence-Stated`, whose text says that no licence
is given for them, and every file of the table above has its third party's
licence and holders, by its own header (`ScriptSite.h`) or by a table of
`REUSE.toml`, which for the sources and generated tables that carry the
project's header adds them beside what the header says.

`build/check-source-tarball.py` requires every notice file of that table's
first four rows in every tarball. `hmailserver/docs/Licenses` also still holds the notices of
components the tarball does not carry - 7-Zip, the MariaDB client, the Fat Cow
icons, zlib as `hMailServer.exe` compiles it, and OpenSSL, Boost and libpq as
the Windows build ships them - which a distribution's copyright file can
describe as licence texts only.

The tarball carries no program and no archive: the checker refuses any file that
begins as a Windows (PE), ELF or Mach-O executable, a Java class or a WebAssembly
module, or as a zip (an empty or a spanned one too), 7-Zip, RAR, ar (a static
library or a `.deb`) or rpm archive, a cabinet, an OLE compound file, a squashfs
image, or a gzip, compress (`.Z`), xz, bzip2, zstd, lz4 or lzip stream, and any
POSIX or GNU tar archive, which says `ustar` at offset 257 (an old v7 tar has no
such mark). The one archive it allowed by name until 23 October 2026, the list
of third-party licences as an OpenDocument spreadsheet (a zip by format), is a
table in Markdown now, `hmailserver/docs/Licenses/License list.md`.

Reproducible
------------

The same commit makes the same bytes wherever git's own tar writer and GNU gzip
make them. Git for Windows 2.50.1, and git 2.53.0 with gzip 1.14 in Ubuntu 26.04,
made one tarball byte for byte on 28 September 2026, and so did Debian's git
2.39.5 and 2.47.3 (trixie's, which the `source-tarball` job installs) with
Debian's gzip 1.10, 1.12 and 1.13; another gzip (busybox's, pigz) compresses the
same tar differently. What makes it so:

- `git archive` writes the tar: the commit's tree in git's order (sorted by name
  within each directory), every entry owned by uid 0 and gid 0 and named root,
  every entry stamped with the commit's time (its `SOURCE_DATE_EPOCH`), and a pax
  header recording the commit, which `git get-tar-commit-id` reads;
- `tar.umask` 0022: files 0644, directories and executables 0755, whatever the
  maker's umask or gitconfig;
- `core.autocrlf` and `core.eol` set to what a Linux clone has, neither the
  system's nor the user's attributes file read, and a clone whose own
  `.git/info/attributes` sets anything refused (git applies that file over the
  commit's, and a `* -text` there made another tarball of the same commit), so
  the line endings are `.gitattributes`' alone;
- `gzip -n -9` with `GZIP` unset: no name and no time in the gzip header.

The `source-tarball` job makes it twice, the second time in another directory
and under another umask, and fails if the two differ; the checker then holds
the owners, the modes, the one time, the gzip header, the list and the licences
to all of the above, and checks the commit the tarball records is the
pipeline's.

Building from it
----------------

`build/ci/linux-build.sh` and `build/ci/linux-suite.sh` take
`HM_SOURCE_TARBALL`: the tarball, or a directory holding exactly one
`hmailserver-*.tar.gz` (the job's artifact directory, `source-tarball`). The
script unpacks it into `HM_SOURCE_DIR` (`/tmp/hm-source`, emptied first, so
never one that holds the directory the script runs in or the tarball, compared
as the file system resolves them, `..` and links included), checks
it with the checker it carries - against `CI_COMMIT_SHA` when that is set, so a
pipeline builds its own commit's tarball and no other - and runs everything
from there: the CMake build, the tester programs, the OpenPGP round trip, the
version stamps, CPack and the packages' SBOMs (`build/native-dependencies.ps1
-LinuxOnly`, which reads none of the Windows build's inputs); for the suite, the
database scripts, the data files and the suite itself. The results go where they
would without it, relative to where the script was started. Without the
variable both scripts build and test the checkout, as before.

By hand, with nothing but the tarball (and the build's own requirements: clang,
CMake, Ninja, Boost, OpenSSL, libpq, OpenLDAP, zlib and GnuPG; Python for the
checker and the packages' SBOMs, and pwsh for the SBOMs' component list), in a
directory where it may leave `linux-build/`:

```
mkdir scripts && tar -xzf hmailserver-<version>.tar.gz -C scripts
HM_SOURCE_TARBALL=hmailserver-<version>.tar.gz \
  bash scripts/hmailserver-<version>/build/ci/linux-build.sh
```

`linux-repro` still builds its second copy from the checkout and compares it
byte for byte with `linux-build`'s, which is then the tarball's: the same
server and packages from both is the proof that nothing left out reaches the
build.

For the distributions
---------------------

The list is written to be read as it is:

- **Debian**, `debian/copyright`: `python3 build/check-source-tarball.py
  --files-excluded` prints the `Files-Excluded` field. Its globs are separated
  by white space, so a space in a path is written `?`; the checker makes sure
  each such pattern matches that path and no other. With the project's own
  tarball the field removes nothing; it is what makes a tarball uscan takes
  from a git tag hold the same files.
- **RPM**, in `%prep`, for a source that is not already the project's tarball:
  `while IFS= read -r p; do case "$p" in ''|'#'*) ;; *) rm -rf -- "$p" ;; esac; done < build/source-exclusions.txt`
  (the list is LF on every platform).
- **Guix**, in the origin's snippet: `delete-file-recursively` for each line
  that is neither blank nor a comment.

What is not done here
---------------------

- The tarball's own SBOM (`.cdx.json`), its line in the sums and its Sigstore
  bundle: the release chain's (`build/ci/release-chain.py`), with the rest of
  the release's files.
- A signature of the maintainers' own: the release's Sigstore bundle, made as
  the tag's pipeline, is its signature, as for every other release file.

What has run
------------

On the laptop on 28 September 2026, before any pipeline:

- `source-tarball.sh` twice in WSL Ubuntu 26.04 (git 2.53.0, gzip 1.14) and once
  under Git for Windows 2.50.1: the same SHA-256 all three times.
- `check-source-tarball.py` on that tarball (passed), on copies of it with
  `libraries/mssqlce3.5/sqlce_oledb.h` or SQL Server Compact's licence planted
  in them (each refused), on the tree (passed), and `--self-test` (every
  negative control reported: a planted excluded path, a missing QuickJS licence,
  a wrong owner, mode, time, top directory, version, gzip header or commit, a
  link, a program, an archive not allowed, and in the tree an unguarded include
  of a left-out header, a left-out file the build reads, a new file of a
  Windows-only tree).
- The tarball's own `build/ci/linux-build.sh` with `HM_SOURCE_TARBALL`, run in an
  empty directory of WSL outside any git tree, with clang 21.1.8: the server,
  every tester program (`spf-tests`, `script-binding-tests`, `snmp-tests`,
  `object-store-signature-tests`), and the `.deb` and the `.rpm` with their SBOMs.
- The same script on a clone of the same commit, without the variable: the same
  server binary, `.deb`, `.rpm` and SBOMs, byte for byte, when both runs are given
  the same `CI_COMMIT_TIMESTAMP` and `CI_COMMIT_SHA`, as a pipeline gives
  `linux-build` and `linux-repro` (the re-check below ran it so; without them the
  clone takes its date from git and the tarball from its files, and the SBOM's
  commit is unknown in the tarball, so the packages differ by those alone).
- In both, the OpenPGP round trip failed alike, and not for the tarball: WSL's
  GnuPG 2.4.8 with libgcrypt 1.12.0 refuses the fixtures' passphrase after the
  agent that imported them is restarted, which `linux-build.sh` does because the
  CI image's GnuPG needs it, and decrypts GnuPG's own committed fixture only when
  the agent is kept. The fixtures in the tarball and in the clone are the same
  bytes. The pipeline, in the CI image, is where that step is proved.
- `LinuxRegressionTests.csproj` restored and compiled from the unpacked tarball
  outside any git tree (the .NET 10.0.303 SDK the tree's `global.json` asks for,
  on Windows: 0 errors), so every linked fixture and embedded message is in it.
  The suite has not run against a server from the tarball: that is
  `linux-suite`'s, in the pipeline.

The re-check, the same day, on the same laptop:

- The tarball of `f5e8dd4c0` made by Debian's git 2.39.5 and 2.47.3 with
  Debian's gzip 1.10, 1.12 and 1.13, unpacked from deb.debian.org's packages in
  WSL: the same SHA-256 as the laptop's (and the same tar inside).
- A clone whose `.git/info/attributes` said `* -text` made another tarball of the
  same commit; the script now refuses such a clone, and switches the system's
  attributes file off.
- `HM_SOURCE_DIR` spelt with `..` as an ancestor of the directory the script ran
  in passed the guard and was emptied, working directory and all (in a scratch
  directory); the guard now compares the resolved paths, and refuses a
  destination that holds the tarball too.
- `check-source-tarball.py` on the tree with one more line each: 26 paths the
  Linux jobs read, of which the first version let through the suite project's
  `Shims`, its lock file, `global.json`, the checker itself, the fuzzing seeds and
  harness, `PKGBUILD`, `SSL examples`, the settings baseline and an OpenPGP
  fixture; each is refused now.
- From the tarball alone in WSL: CMake configured, and the six sources that
  include `zlib.h` compiled against `/usr/include/zlib.h` (ninja's dependency
  record). cppcheck 2.19.0 and clang-tidy 21.1.8, run on the six as the
  `static-analysis` job runs them, found the same findings (rule, file, line
  text) before and after the change of include, so the release's security gate
  sees nothing new.
- `linux-build.sh` from the tarball and from a clone, both with the pipeline's
  `CI_COMMIT_TIMESTAMP` and `CI_COMMIT_SHA`: the binary, the `.deb`, the `.rpm`
  and both SBOMs byte for byte the same (the OpenPGP round trip failing in both,
  as above).
