The command-line clients as packages
====================================

The two command-line clients of [CommandLine.md](CommandLine.md) - `hmctl` and
the `HmailServer` PowerShell module - are packaged for the two places an
administrator installs such a thing from without the server:

| Package | Where | Installs | Built from |
|---|---|---|---|
| `hmailserver-cli` | PyPI | the `hmctl` command, with `pipx install hmailserver-cli` (or `pip`) | `hmailserver/source/Tools/Cli/hmctl` and `hmailserver/source/Tools/Cli/python/` |
| `HmailServer` | the PowerShell Gallery | the module, with `Install-PSResource HmailServer` (PowerShell 7.4 and later) or `Install-Module HmailServer` (Windows PowerShell 5.1) | `hmailserver/source/Tools/Cli/HmailServer.psm1` and its manifest `HmailServer.psd1` |

**Neither is published yet.** Both are built and checked in CI now, and each is
published by a job that stays dormant until its account exists ([What the owner
sets](#what-the-owner-sets)). The Gallery is sent the module as a release
signs it: from 6.3.6 every release carries `HmailServer.psd1` and
`HmailServer.psm1` Authenticode-signed, each with its Sigstore bundle
([Publishing](#publishing), step 5). Until then, `hmctl` comes with the `.deb` and
the `.rpm` as `/usr/bin/hmctl`, and the module is the file in the tree.

Both packages are at the server's version: `hmailserver-cli` 6.3.6 and
`HmailServer` 6.3.6 are the clients as the 6.3.6 release has them. They speak the
REST API, so either works against any server whose API has the routes it asks for;
`hmctl api` and `Invoke-HmApi` reach a route newer than the client.

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
`/usr/bin/hmctl` does. `hmctl --version` says the server's version, `hmctl 6.3.6`, as
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

`.gitlab/ci/cli-packages.yml`, on GitLab's shared runners:

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

deps.dev and OpenSSF Package Analysis
-------------------------------------

Both come with the PyPI package and need nothing else from the project:

* **deps.dev** indexes PyPI. A project appears there only when a package links to
  it: the package's project URLs name this project (an unverified link), and the
  attested upload links the two as `PYPI_PUBLISH_ATTESTATION`. After the first
  upload, `api.deps.dev/v3/projects/gitlab.com%2Fhmailserver%2Fhmailserver` should
  answer with `hmailserver-cli` linked, which is the plan's W7.1 to check. deps.dev
  shows no Scorecard for a GitLab project.
* **OpenSSF Package Analysis** installs every new PyPI upload in a sandbox, imports
  and runs it, and records the files, addresses and commands it touches. There is nothing
  to opt into; what the project does is keep the package free of install-time and
  import-time behaviour, which `build/check-cli-packages.py` holds. A rehearsal with
  the analysis image before the first upload is the plan's W7.1.

Nothing else the project ships - the installer, the `.deb`, the `.rpm`, the image,
the chart or the PowerShell module - is analysed by either.
