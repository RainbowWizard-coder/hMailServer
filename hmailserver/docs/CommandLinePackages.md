The command-line clients as packages
====================================

The two command-line clients of [CommandLine.md](CommandLine.md) - `hmctl` and
the `HmailServer` PowerShell module - are packaged for the two places an
administrator installs such a thing from without the server:

| Package | Where | Installs | Built from |
|---|---|---|---|
| `hmailserver-cli` | PyPI | the `hmctl` command, with `pipx install hmailserver-cli` (or `pip`) | `hmailserver/source/Tools/Cli/hmctl` and `hmailserver/source/Tools/Cli/python/` |
| `HmailServer` | the PowerShell Gallery | the module, with `Install-PSResource HmailServer` (PowerShell 7.4 and later) or `Install-Module HmailServer` (Windows PowerShell 5.1) | `hmailserver/source/Tools/Cli/HmailServer.psm1` and its manifest `HmailServer.psd1` |

**Both are published, since 21 October 2026,** each by its job in the `master`
pipeline 2966261322 ([Publishing](#publishing)):

* `hmailserver-cli` 6.3.7 is on PyPI
  (`https://pypi.org/project/hmailserver-cli/6.3.7/`): `pypi-publish` (job
  17306495320) rebuilt it from the signed tag `v6.3.7` and uploaded the wheel and the
  sdist at 00:30 UTC, and PyPI gives each file one publish attestation, from the
  GitLab trusted publisher (`hmailserver/hmailserver`, `.gitlab-ci.yml`, environment
  `pypi`).
* `HmailServer` 6.3.7 is on the PowerShell Gallery
  (`https://www.powershellgallery.com/packages/HmailServer/6.3.7`), under the owner
  `progressiverobot`: `psgallery-publish` (job 17306495321) verified the release's
  Authenticode-signed `HmailServer.psd1` and `HmailServer.psm1` against their Sigstore
  bundles and published them at 01:26 UTC. The Gallery scans a new version before it
  lists it; ten minutes later its feed and its search gave 6.3.7 as the latest
  version, and the package it served matched the SHA-512 it publishes for it and held
  the two files with the SHA-256s the job had verified.

Read that day, both listings show the texts 6.3.7 was published with, which name
neither the fork nor Progressive Robot: PyPI's summary is "hmctl: administer an
hMailServer over its REST API from a command line", and the Gallery's description
opens "Administer an hMailServer". A published version keeps its texts; the next
release carries the ones the tree has now ([Whose clients these
are](#whose-clients-these-are)). A version a channel has is never sent again
([Publishing](#publishing), step 3). `hmctl` also comes with the `.deb` and the
`.rpm` as `/usr/bin/hmctl`, and every release from 6.3.6 carries the module's two
files Authenticode-signed, each with its Sigstore bundle ([Publishing](#publishing),
step 5).

Both packages are at the server's version: `hmailserver-cli` 6.3.7 and
`HmailServer` 6.3.7 are the clients as the 6.3.7 release has them. They speak the
REST API, so either works against any server whose API has the routes it asks for;
`hmctl api` and `Invoke-HmApi` reach a route newer than the client.

On 23 October 2026 both were installed from there on clean Linux and Windows, as a
user installs them, and administered a 6.3.7 server over TLS ([Installed from PyPI
and the Gallery](#installed-from-pypi-and-the-gallery)); macOS was not tried.

Whose clients these are
-----------------------

The original hMailServer, Martin Knafve's, is developed again (5.7.0 on 28 September
2026, 5.7.1 and a 5.7.2 pre-release on 29 September, at
`github.com/hmailserver/hmailserver`), and it has no REST API: programs administer
it through its COM API, and neither client works with it. So each text a listing
shows says first that these are the clients of Progressive Robot's fork of
hMailServer, for the fork's REST API, and not of the original:

| Listing | Text | Says |
|---|---|---|
| PyPI's summary | `hmailserver/source/Tools/Cli/python/pyproject.toml`'s `description` | "hmctl, the command-line client of Progressive Robot's fork of hMailServer, for the fork's REST API - not the original hMailServer, which has none" |
| PyPI's description | `hmailserver/source/Tools/Cli/python/README.md`, before its Install section | the same, and that `hmctl` does not work with the original |
| The Gallery's description | `hmailserver/source/Tools/Cli/HmailServer.psd1`'s `Description`, its first sentence | "The PowerShell client of Progressive Robot's fork of hMailServer, for the fork's REST API - not the original hMailServer by Martin Knafve, which has none" |
| The Gallery's tags | `HmailServer.psd1`'s `PrivateData.PSData.Tags` | `ProgressiveRobot` among them |

`build/check-cli-packages.py` refuses a summary, a README or a `Description` that
does not say so, and a summary that is not one line of at most 512 characters, which
is all PyPI takes. 6.3.7 was published before they said so, and keeps the texts it
has.

One hmctl
---------

The package does not have a copy of `hmctl` in the tree. It is made when it is
built, by `build/ci/cli-packages.py build`, in a staging directory:

| Staged file | Where it comes from |
|---|---|
| `pyproject.toml`, `README.md` | `hmailserver/source/Tools/Cli/python/`, with LF line endings |
| `hmailserver_cli/hmctl.py` | `hmailserver/source/Tools/Cli/hmctl`, byte for byte: the file CMake installs as `/usr/bin/hmctl` in the `.deb`, the `.rpm` and the Arch package |
| `hmailserver_cli/__init__.py` | written by the script: `__version__` is `HMAILSERVER_VERSION` from `hmailserver/source/Server/Common/Application/Version.h` |
| `hmailserver_cli/__main__.py` | written by the script: `python -m hmailserver_cli` runs `hmctl` |
| `LICENSE` | the repository's, with LF line endings |

`python -m build` then makes the sdist and, from the sdist, the wheel, so the sdist
is shown to hold everything the package needs. The backend is `flit_core`, which is
declarative: nothing of the project's runs when the package is built or installed.
`SOURCE_DATE_EPOCH` is the tree's last commit's time unless it is set, so the same
tree makes the same bytes; the `cli-packages` job builds twice and compares them.
Measured on 28 September 2026 on one tree: the sdist was the same bytes built on
Windows and on Linux, and the wheels differed only in the byte of each zip entry that
says which system made it, which Python's `zipfile` takes from the platform. Built
without git history and without `SOURCE_DATE_EPOCH`, the files carry the time they
are made at, and the builder says so.

The command is the console script `hmctl = hmailserver_cli.hmctl:main`, so pipx and
pip make an `hmctl` (an `hmctl.exe` on Windows) that calls the same `main()`
`/usr/bin/hmctl` does. `hmctl --version` says the server's version, `hmctl 6.3.7`, as
the package's version does (`pipx list`, `pip show hmailserver-cli`), and its requests
name it in their `User-Agent`. Until 6.3.6 it said `hmctl 1.0`, the client's own first
version, which never moved. `VERSION` in `hmctl` is a literal, since the script runs
as it is from `/usr/bin` with no `Version.h` beside it, so it is stamped with the
release (RELEASE.md, step 7).

`build/check-cli-packages.py` holds the package to that shape, in the `checkers` job
of every push's and merge request's pipeline:

* `pyproject.toml` writes no version (the version is derived, never stamped), declares
  no dependency, no optional dependency and no entry point but `hmctl`, builds with
  `flit_core` in a range that holds the version the jobs pin, and names this project
  for every URL;
* `python/` holds `pyproject.toml` and `README.md` and nothing else: no `setup.py` or
  `setup.cfg`, no `.pth` file, and no committed copy of `hmctl`;
* `hmctl` imports the standard library alone and no module of it newer than Python 3.8
  (`zoneinfo`, `graphlib`, `tomllib` and the later ones), parses with Python 3.8's
  grammar (as far as Python's `ast` checks a grammar version), has a `main()`, and runs nothing
  when it is imported beyond defining its names. The package imports it as a module,
  and OpenSSF Package Analysis installs every new PyPI upload in its sandbox and
  imports it, so an import defines things and does nothing else;
* `hmctl`'s `VERSION` is `Version.h`'s version;
* with `--dist`, the built sdist and wheel hold exactly the files above at
  `Version.h`'s version, `hmctl.py` byte for byte, no `Requires-Dist`, and the one
  console script.

Tested on 28 September 2026: the wheel installed with pipx 1.8.0 on Python 3.13 on
Windows, and into clean environments on Python 3.10 on Windows and Python 3.14 on
Ubuntu 26.04, each `hmctl` listing a test server's domains. Python 3.8, the floor
`requires-python` states because `hmctl` states it, on 28 September 2026 on Windows
(Python 3.8.10, python.org's build as nuget.org serves it): the wheel installed with
3.8's pip 21.1, `python -m hmailserver_cli` ran, and `build/check-cli.py --python`
passed its 1,120 checks with `hmctl` under 3.8. macOS was not tried.

The module's manifest
---------------------

`HmailServer.psd1` is what the Gallery lists and what `Import-Module` reads first:

* `ModuleVersion` is the server's version. It is a literal, since a data file cannot
  read `Version.h`, so it is stamped with the release (RELEASE.md, step 7), and
  `build/check-cli-packages.py` fails the `checkers` job while it differs.
* `FunctionsToExport` is the list the module's own `Export-ModuleMember` names,
  written out rather than `*`, so that PowerShell knows the commands without loading
  the module; `CmdletsToExport`, `VariablesToExport` and `AliasesToExport` are empty.
  The checker fails when the two lists differ, when the manifest names a function the
  module does not define, or when the module exports anything but functions. A new
  exported function goes into both.
* `PowerShellVersion` 5.1 and `CompatiblePSEditions` `Desktop` and `Core`: Windows
  PowerShell 5.1, and PowerShell 7 on Windows and Linux.
* `ProjectUri` and `LicenseUri` are this project's; the `GUID` never changes.
* The manifest and the module are ASCII, with no byte order mark, and the checker
  fails on any other byte. A release signs both with Authenticode, and Windows
  PowerShell 5.1 reads a script file without a byte order mark in the machine's
  own ANSI code page, which is the text the signature is checked over: a character
  beyond ASCII would read as another on a machine with another code page, and the
  signature would not match there. An em dash in a comment is enough.

PSScriptAnalyzer runs every default rule over the manifest and the module, with no
settings file, and finds nothing. Where a rule was answered by the code, the code
changed: `[OutputType()]` on the three functions whose answers it could type, help on
the two exported functions that had none, a statement in each catch block that meant
"nothing to do", and the two argument completers reading `$args` rather than
declaring five parameters to use one or two. Where a rule was answered by a reason,
the reason is a `SuppressMessageAttribute` on that one function: `-Password` on
`New-HmAccount` also takes a plain string, `-Event` on `Test-HmWebhook` and
`-Credential` on `Revoke-HmDevice` (a switch) are published parameter names,
`Remove-HmAddressTraces` is a published command name, and three internal helpers
keep their names.

Tested on 28 September 2026: `Test-ModuleManifest`, `Import-Module` through the
manifest, PSScriptAnalyzer 1.25.0, `Connect-HmServer`, `Get-HmDomain` and the domain
and address completion against a test server, on Windows PowerShell 5.1 and
PowerShell 7.5 on Windows and PowerShell 7.6 on Ubuntu 26.04.

`-Insecure`, which `Connect-HmServer` and `Invoke-HmApi` take for the loopback alone,
skips the certificate check of that server's calls on both. On PowerShell 7 it is
`-SkipCertificateCheck`, and such a call follows no redirect, since the check would
be skipped wherever it led. Windows PowerShell 5.1 has no `-SkipCertificateCheck`:
what judges a certificate there is the process's
`ServicePointManager.ServerCertificateValidationCallback`, so for an `-Insecure` call
the module puts in its place, for that call alone, one that accepts the certificate
of the loopback address and port the session names, sent there and answered from
there, and hands every other connection to the callback it replaced (or, with none,
to the system's own verdict); it puts the first back when the call ends. Until 6.3.6
`-Insecure` had no effect on 5.1, so a server with a self-signed certificate was
reached from 5.1 only once the certificate was trusted. Tested on 2 October 2026
against a loopback server with a self-signed certificate, on Windows PowerShell 5.1
and PowerShell 7.5: the certificate refused without `-Insecure` and taken with it,
the process's callback as it was after the call, a request of the session outside
the module still refused, `-Insecure` for a remote host refused by `Invoke-HmApi`
too, and a redirect to another port of the loopback refused.

Installed from PyPI and the Gallery
-----------------------------------

On 23 October 2026, 6.3.7 of both packages, installed as a user installs them, each
into a clean container or a clean Windows installation, against one test server:
6.3.7's container image (`registry.gitlab.com/hmailserver/hmailserver`, the index
`sha256:2a5de4ebd04cd1e39f69ebeca3e905dd80217c3178132bd17a07880845ae629f`) beside
PostgreSQL 18, started as its start test starts it ([ContainerImage.md](ContainerImage.md),
"The start test"), which points the REST listener at the self-signed pair
`--apply-new-install-defaults` makes for the mail listeners (`HM_REST_CERTIFICATE_FILE`
and `HM_REST_PRIVATE_KEY_FILE`), with the host name `mail.example.test`, so that the
certificate names that host (`CN=mail.example.test`, `DNS:mail.example.test`,
`CA:FALSE`). Its REST API answered as `6.3.7-B46`, reached
from Linux over a Docker network and from Windows over the network by that name in
the hosts file. Every client was run with the certificate untrusted, which it had to
refuse, then trusted as [CommandLine.md](CommandLine.md) says ("Credentials").

- **PyPI's provenance**, read in a clean `python:3.14-slim-trixie` container. PyPI's
  JSON API lists the wheel `hmailserver_cli-6.3.7-py3-none-any.whl` (55,833 bytes,
  SHA-256 `c217980b094df00a7c1d20bb036e10c168a208a7269df6ba21675ac27a4965d5`) and the
  sdist `hmailserver_cli-6.3.7.tar.gz` (55,196 bytes,
  `bad7f4baecf3a2eeeab29eca507b50526282caa613915c1b07eb1746b727c356`), no
  `Requires-Dist`, `requires_python` `>=3.8`; each file downloaded had that SHA-256.
  PyPI's integrity API gives each one attestation bundle, from the publisher `GitLab`,
  repository `hmailserver/hmailserver`, workflow file `.gitlab-ci.yml`, environment
  `pypi`, holding one publish attestation whose subject is that file at that digest;
  its Sigstore certificate names
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/heads/master`, the
  project's id `86730559` and job 17306495320. `pypi-attestations` 0.0.30 (the
  version `build/ci/cli-packages-requirements.txt` pins, installed from it) verified
  both files, as `pypi:<file name>` and as the files downloaded, with `verify pypi
  --repository https://gitlab.com/hmailserver/hmailserver`; it refused another
  repository ("provenance was signed by repository "hmailserver/hmailserver",
  expected ...") and the wheel with one byte added ("subject does not match
  distribution digest").
- **pipx on Linux**: `python:3.14-slim-trixie` (Debian 13.7, Python 3.14.8, pip
  26.2.1 alone installed), `pip install pipx` (1.18.4), `pipx install
  hmailserver-cli==6.3.7`: its environment held `hmailserver-cli` 6.3.7 and pip and
  nothing else, its `hmctl.py` had the SHA-256 of the `v6.3.7` tag's `hmctl`, and
  `hmctl --version` said `hmctl 6.3.7`. Untrusted, `hmctl domain list` said "TLS
  refused the server's certificate: [SSL: CERTIFICATE_VERIFY_FAILED] certificate
  verify failed: self-signed certificate" (exit 1), and `--insecure` was refused for
  that host (exit 1). Trusted with `SSL_CERT_FILE`, and then in the system's store
  (`update-ca-certificates`) without it: `domain list` (none), `domain create --set
  name=example.com --set active=true`, `account create --domain example.com
  --from-json -` (the password on standard input), and `domain list`, `domain get`,
  `account list` and `account get` read both back; by the server's address, "IP
  address mismatch" (exit 1); a wrong password, exit 3; an account that is not
  there, exit 4.
- **pip on Linux**: the same image, `python -m venv`, `pip install
  hmailserver-cli==6.3.7`, which fetched the wheel and nothing else: `hmctl` and
  `python -m hmailserver_cli` said 6.3.7 and listed the domain and the account, and
  `hmctl --json account get` gave the account with no field of a password.
- **Ubuntu's own pipx**: `ubuntu:26.04` (26.04.1), `apt-get install pipx` (1.8.0-1),
  `pipx install hmailserver-cli==6.3.7` (Python 3.14.4), the certificate in the
  system's store: `domain list` and `account list`.
- **pipx on Windows**: Windows Server 2025 (build 26100), a clean installation with
  nothing else installed. Python 3.14.9 from python.org (the SHA-256 python.org
  publishes for `python-3.14.9-amd64.exe`, Authenticode Valid, signed by the Python
  Software Foundation), installed for the user; `python -m pip install --user pipx`
  (1.18.4), `python -m pipx install hmailserver-cli==6.3.7`: `hmctl.exe`, `hmctl
  6.3.7`. Untrusted, the same refusal as on Linux; with the certificate in the
  machine's Trusted Root store (`Import-Certificate ... Cert:\LocalMachine\Root`),
  `domain list` and `account list`.
- **The Gallery on Windows PowerShell 5.1**: the same machine (5.1.26100.7462,
  PowerShellGet 1.0.0.1 and PackageManagement 1.0.0.1, execution policy
  RemoteSigned). `Install-Module Microsoft.PowerShell.PSResourceGet -Scope CurrentUser
  -Force` stopped at PowerShellGet 1.0.0.1's question whether to install the NuGet
  provider, which a session with no console cannot answer ("Exception calling
  "ShouldContinue" with "2" argument(s)"); at a console it is answered Y. With
  `Install-PackageProvider -Name NuGet -MinimumVersion 2.8.5.201 -Scope CurrentUser
  -Force` first (2.8.5.208), it installed PSResourceGet 1.2.0, and `Install-PSResource
  HmailServer -Version 6.3.7 -TrustRepository` installed `HmailServer` 6.3.7 by
  "Christopher Holloway / Progressive Robot Ltd" into the user's
  `Documents\WindowsPowerShell\Modules`. `Get-AuthenticodeSignature` of
  `HmailServer.psd1` and `HmailServer.psm1`: Valid, signed by `CN=Progressive Robot
  Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West and Chester, C=GB`,
  countersigned by the Microsoft Public RSA Time Stamping Authority;
  `Test-ModuleManifest`: 95 functions. `Connect-HmServer -Url
  https://mail.example.test:<port> -Password <a SecureString>` and `Get-HmDomain`:
  untrusted, "GET /api/v1/domains failed." alone, the reason lost (the fix is below);
  trusted, `example.com`, and `Get-HmAccount -Domain example.com` gave the account.
  `Uninstall-PSResource`, then `Install-Module HmailServer -RequiredVersion 6.3.7
  -Scope CurrentUser -Force` with PowerShellGet 1.0.0.1, installed and worked the
  same.
- **The Gallery on PowerShell 7 on Windows**: PowerShell 7.6.8 from its GitHub release
  (`PowerShell-7.6.8-win-x64.msi`, the SHA-256 GitHub gives for the asset,
  Authenticode Valid, signed by Microsoft Corporation), whose PSResourceGet is 1.2.0:
  `Install-PSResource HmailServer -Version 6.3.7 -TrustRepository` into
  `Documents\PowerShell\Modules`, both files Valid as above; untrusted, "The SSL
  connection could not be established, see inner exception." ("The remote
  certificate is invalid because of errors in the certificate chain:
  UntrustedRoot"); trusted, `Get-HmDomain` and `Get-HmAccount` as above. Installed
  again with `-Reinstall -AuthenticodeCheck`, which checks the files' signatures, it
  installed.
- **The Gallery on PowerShell 7 on Linux**: Microsoft's image
  `mcr.microsoft.com/powershell:7.5-ubuntu-24.04` (its newest stable tag, built on 30
  January 2025: PowerShell 7.5.0, PSResourceGet 1.1.0, PowerShellGet 2.2.5), one
  clean container with `Install-PSResource HmailServer -Version 6.3.7
  -TrustRepository` and another with `Install-Module HmailServer -RequiredVersion
  6.3.7 -Scope CurrentUser -Force`: both 6.3.7, `Test-ModuleManifest` 95 functions.
  `Get-AuthenticodeSignature` is Windows' alone; the two files had the SHA-256s they
  had on Windows (`a87db2cd648af9ce7e9e0b0ccfe438897428273e7d2f91c61af8e2dfb3c42865`
  and `8c5cd591cfbcc07a812fe48e506d89b672c22432d06e71aa3e0256bb5a2cdc47`), which are
  those of the release's own signed `HmailServer.psd1` and `HmailServer.psm1` in its
  package `hmailserver/6.3.7`. Untrusted, UntrustedRoot as on Windows; with the
  certificate in the system's store, `Get-HmDomain` and `Get-HmAccount`.

What the runs found, and the tree now has: the module under Windows PowerShell 5.1
gave no reason when it got no answer at all - a certificate the machine does not
trust, nothing listening on the port - and said "GET
/api/v1/domains failed." alone, where PowerShell 7 gives the system's reason; and
under both, an answer that was not JSON, a proxy's text, lost its text. From the
release after 6.3.7 it says "GET /api/v1/domains failed: The underlying connection
was closed: Could not establish trust relationship for the SSL/TLS secure channel;
The remote certificate is invalid according to the validation procedure." there,
which the module of the tree said on the same machine against the same server, and
"failed (502): " and the text for the second, as `hmctl` prints it (and for JSON
without `error`, its `message` or the status alone, as `hmctl` does);
`build/check-cli.py` holds both, under every PowerShell the machine running it has,
with an untrusted certificate of its own on a loopback listener. Run with Windows
PowerShell 5.1 taking the module's whole half, the check found two more, which the
tree has fixed too: `Set-HmOpenPgpKey -Path` sent nothing there, and `hmctl` refused
the document `Export-HmConfiguration` writes there, which begins with a byte order
mark. Neither client's documents said how a REST listener that another machine
connects to is given a certificate, or how a server's self-signed certificate is
trusted on that machine; [CommandLine.md](CommandLine.md) now does. Not tried:
macOS, for want of a Mac.

Building and checking by hand
-----------------------------

The tools are the ones the jobs use, pinned with their hashes in
`build/ci/cli-packages-requirements.txt` (made from `cli-packages-requirements.in`,
which says how to move a version), all from PyPI:

```sh
python -m venv ~/.venvs/hm-cli
~/.venvs/hm-cli/bin/pip install --require-hashes -r build/ci/cli-packages-requirements.txt
~/.venvs/hm-cli/bin/python build/ci/cli-packages.py build --out cli-packages
~/.venvs/hm-cli/bin/python -m twine check --strict cli-packages/dist/*
~/.venvs/hm-cli/bin/python build/check-cli-packages.py --dist cli-packages/dist
pipx install cli-packages/dist/hmailserver_cli-*.whl
```

(`%USERPROFILE%\.venvs\hm-cli\Scripts\python` on Windows.) The environment is
outside the checkout: in it, `build/add-license-headers.py --check` would read every
file of it, and `git status` would list it. `cli-packages/` is ignored and holds only
the package's own files. The module:

```powershell
pwsh -NoProfile -File build/ci/cli-powershell.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build/ci/cli-powershell.ps1
```

`cli-powershell.ps1` runs `Test-ModuleManifest`, imports the module through its
manifest, and runs PSScriptAnalyzer, which it downloads from the PowerShell Gallery at
a pinned version and holds to the SHA-512 the Gallery published for that package
before it loads it. `build/check-cli.py` is still the test of what the two clients do:
the `checkers` job runs its `hmctl` half (`--python`) in every pipeline, and
`cli-powershell` its PowerShell half (`--powershell`), which fails there, rather than
skipping, when pwsh cannot run it.

The jobs
--------

`.gitlab/ci/cli-packages.yml`, on the runner `.gitlab-ci.yml`'s `default:` gives a job: linix, server2 or s1-linux for
`master`, a `batch*` branch or a `v*` tag, GitLab's hosted runner for a merge request or any other branch
([ContinuousIntegration.md](ContinuousIntegration.md), "Every job on the project's own runners"):

| Job | When | What |
|---|---|---|
| `cli-packages` | a merge request's or a branch's pipeline that changes the clients or their packaging; never a tag's | Both checkers' self-tests, the sdist and the wheel built, built again and compared, held to the tree, `twine check --strict`, and the wheel installed alone (`--no-index`) into a clean environment where `hmctl` and `python -m hmailserver_cli` run and `hmctl` refuses cleanly with no server there |
| `cli-powershell` | the same | `build/ci/cli-powershell.ps1` under pwsh on Linux, in Microsoft's .NET SDK image, then the module against `build/check-cli.py`'s recorded server (`--powershell`) |
| `pypi-publish` | a `master` pipeline, started by hand | The release's `hmailserver-cli` to PyPI ([Publishing](#publishing)) |
| `psgallery-publish` | a `master` pipeline, started by hand | The release's module to the PowerShell Gallery |

Publishing
----------

The publish jobs publish a release, not the commit their pipeline is for. In order:

1. **Set up at all?** Asked first, so that an unconfigured job says so and passes
   whatever else is true.
   * `pypi-publish` exchanges the job's ID token (audience `pypi`) at
     `pypi.org/_/oidc/mint-token` for a short-lived upload token. Until the trusted
     publisher exists, PyPI answers `invalid-publisher`, and the job says "not
     configured" and passes. PyPI gives the same answer when a publisher exists and
     no longer matches (the project moved, the file or the environment renamed), so
     once PyPI has a project `hmailserver-cli` that answer fails the job instead; a
     *pending* publisher that does not match is still reported as "not configured",
     since PyPI shows nothing of it.
   * `psgallery-publish` says "not configured" and passes while `PSGALLERY_API_KEY`
     is not set.
2. **Which release.** The project's CI/CD variable `HM_CHANNEL_VERSION` when it is set
   (`6.3.6`), otherwise the update feed's `releases/latest`. Either way the feed must
   list it (`releases/tags/v<version>`) as a published release and not a draft or a
   pre-release: a signed tag whose release pipeline failed or has not finished is no
   release, and what a channel has had it keeps. The project refuses variables given
   when a pipeline or a job is started, so the choice is a project variable.
3. **Never twice.** A version PyPI or the Gallery has already is not sent again; the
   job says so and passes. PyPI never takes a file name twice in any case. twine sends
   one file at a time, so a failed upload can leave PyPI with one of the two; the next
   run builds the tag again and sends the other alone.
4. **Its tag.** `v<version>` must be an annotated tag on `master`, signed by the one
   key `master`'s `.gitlab/allowed_signers` lists: the release job's own check
   (`build/ci/release-chain.py`), run again. A release made before the packages
   existed (6.3.5 and older) passes this and is refused at the next step, with the
   reason.
5. **Its tree.** The package is built from that tag's tree, with the tag commit's
   time as `SOURCE_DATE_EPOCH`, and held to the version; the module's two files are
   taken from it and its manifest held to the version. Then the module's two files
   as the release carries them: `release` collects them from the tagged tree,
   unsigned, and `release-authenticode` signs them with Authenticode, by the same
   Azure Artifact Signing profile and pin as the installers, so `release-sign` signs
   the signed bytes with Sigstore ([ContinuousIntegration.md](ContinuousIntegration.md),
   "The release"). `psgallery-publish` downloads each, and its `.cosign.bundle`, from
   the release's package `hmailserver/<version>`; verifies the bundle with
   `sigstore` (hashed in `build/ci/cli-packages-requirements.txt`, installed into a
   virtual environment of the job's own) as signed by the tag's pipeline,
   `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
   for this repository (`86730559`) at the tag's commit, and recorded in Sigstore's
   public log; and requires the file to be the tag's own bytes with one signature
   block appended and nothing else. The Gallery keeps a version for good, so a
   release without the signed module (6.3.5 and before), a bundle that does not
   verify, or a file that is not the tag's own is refused here, before anything is
   sent. A user who downloads the two files from the release checks them the same
   ways: `Get-AuthenticodeSignature .\HmailServer.psm1` (Valid, signed by
   Progressive Robot Ltd), and `cosign verify-blob` with the file's bundle as
   [SECURITY.md](../../SECURITY.md) shows for the installer.
6. **Signed as this pipeline.** `pypi-publish` signs each file with Sigstore (ID
   token audience `sigstore`) as a PyPI publish attestation (`pypi-attestations`),
   and `twine upload --attestations` sends each file with its attestation. PyPI
   accepts them only from the trusted publisher, and they are what gives deps.dev a
   verified link from the package to this project (`PYPI_PUBLISH_ATTESTATION`). The
   job then reads PyPI back: the files with the bytes uploaded, and their provenance.
   `psgallery-publish` sends the module's folder with `Publish-PSResource`; the
   Gallery scans a new version before it lists it.

The upload token never leaves the job's process and its twine; the Gallery key is
read by `Publish-PSResource` from the job's environment. Neither is printed or put on
a command line.

What the owner sets
-------------------

**PyPI** (Roadmap3, "PyPI"): an account with two-factor authentication, then a
*pending* trusted publisher, which PyPI turns into the project on the first upload:

| Field | Value |
|---|---|
| PyPI Project Name | `hmailserver-cli` |
| Namespace | `hmailserver` |
| Project name | `hmailserver` |
| Top-level pipeline file path | `.gitlab-ci.yml` |
| Environment name | `pypi` |

The namespace and project are the project's path when the job runs:
`hmailserver/hmailserver` since the move into the group on 29 September 2026
(`Progressiverobot/hmailserver` before it). PyPI checks the ID token's `project_path`, its `ci_config_ref_uri`, which
names `.gitlab-ci.yml` for a job of an included file too, and its `environment`. The
GitLab environment `pypi` is made by the job's first run. On GitLab's Free tier an
environment cannot be protected (that is Premium), so what holds the job is its rules:
it runs only in a `master` pipeline, where only those who may merge to `master` can
start a manual job.

**The PowerShell Gallery** (Roadmap3, "The PowerShell Gallery"): a Microsoft account,
then an API key (at most 365 days) with the scope "Push new packages and package
versions" for the glob `HmailServer`, stored as the project's CI/CD variable
`PSGALLERY_API_KEY`, protected and masked. After the first publish the key can be
narrowed to "Push only new package versions". The key expires within a year and is
renewed there.

Optional: `HM_CHANNEL_VERSION` as a project variable, to publish a release other than
the feed's latest.

Both were set up by 21 October 2026, when both jobs published 6.3.7 (above).

deps.dev and OpenSSF Package Analysis
-------------------------------------

Both come with the PyPI package and need nothing else from the project:

* **deps.dev** indexes PyPI. A project appears there only when a package links to
  it: the package's project URLs name this project (an unverified link), and the
  attested upload links the two as `PYPI_PUBLISH_ATTESTATION`. On 21 October 2026,
  after the first upload,
  `api.deps.dev/v3/projects/gitlab.com%2Fhmailserver%2Fhmailserver:packageversions`
  answered with `hmailserver-cli` 6.3.7 linked that way, its attestation verified,
  naming the commit of the `master` pipeline that published it. deps.dev shows no
  Scorecard for a GitLab project. Its unverified links for 6.3.7 were wrong:
  `api.deps.dev/v3/systems/pypi/packages/hmailserver-cli/versions/6.3.7` gave, on
  24 October 2026, the Changelog URL, `.../-/blob/master/CHANGELOG.md`, as the
  source repository (`gitlab.com/hmailserver/hmailserver/-/blob/master/changelog.md`)
  and the Issues URL as the issue tracker of a repository
  `gitlab.com/hmailserver/hmailserver/-`, and left out the Source URL. deps.dev can
  take the source repository from any URL of a package on gitlab.com, and reads a
  path under `/-/` as part of a repository's name: of the other packages on PyPI
  whose URLs have the same shape, deps.dev links some to their repository and some
  to their changelog or their issues. So from the
  release after 6.3.7 the package's project URLs are the project's address alone,
  as Homepage and Source, and the README PyPI shows links the documents, the
  changelog and the issues; `build/check-cli-packages.py` holds the two URLs.
* **OpenSSF Package Analysis** installs every new PyPI upload in a sandbox, imports
  and runs it, and records the files, addresses and commands it touches. There is nothing
  to opt into; what the project does is keep the package free of install-time and
  import-time behaviour, which `build/check-cli-packages.py` holds. The first upload,
  on 21 October 2026, was made without a rehearsal with the analysis image.

Nothing else the project ships - the installer, the `.deb`, the `.rpm`, the image,
the chart or the PowerShell module - is analysed by either.
