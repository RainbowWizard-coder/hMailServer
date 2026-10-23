Package managers
================

Installing hMailServer with `winget` or `choco`, and — for the maintainer — how
the manifests behind those two commands are made, checked and submitted. On
Linux, `apt` and `dnf` install it from the project's own signed repositories
([apt and dnf](#apt-and-dnf), below).

Where this stands today
-----------------------

This project is **Progressive Robot's fork of hMailServer**, not the original
hMailServer, which is Martin Knafve's. The original is developed again - it
released 5.7.0 on 28 September 2026, and 5.7.1 and a 5.7.2 pre-release on 29
September - and packages of it exist, so every package of this project says
in its title or name and in its description that it is the fork, and never takes
an identifier that is the original's.

**Chocolatey: 6.3.7 is in moderation again, without the credit yet.** The
`chocolatey-publish` job pushed `hmailserver-progressiverobot` 6.3.7 at 23:47 UTC
on 20 October 2026, and it passed Chocolatey's automatic checks. That package was
titled "hMailServer" alone, with a summary that did not say fork, and its authors
and copyright named Progressive Robot alone. At 11:31 UTC on 21 October 2026 a
moderator returned it ("Waiting for Maintainer to take corrective action"),
asking that the package's `<authors>` also credit the original hMailServer's
author, Martin Knafve, as this is a fork, and that the same version be
resubmitted. At 01:06 UTC on 22 October 2026 `chocolatey-publish` (job
17362512835, in `master`'s pipeline 2973751489) sent 6.3.7 again in its place,
with the fork's title, summary and description but with authors and copyright
that still name Progressive Robot alone, so it says that it is the fork but does
not yet credit Martin Knafve. It passed Chocolatey's automated validation at 01:39
UTC; its automated verification failed at 01:44 UTC because the verifier's
download of it from Chocolatey's own feed timed out (a gateway timeout, not a
fault in the package), and its page again shows "Waiting for Maintainer to take
corrective action". The community repository's feed still lists it as
`Submitted`, not approved (read at 02:29 UTC that day). The templates now credit
Martin Knafve in the authors and the copyright: once that is on master, another
press of `chocolatey-release` in master's pipeline sends 6.3.7 a third time, with
the credit, in the 22 October package's place, while the feed still lists it as
`Submitted` ([Channels.md](Channels.md), "How a channel job publishes"); the new
package is validated and verified afresh, its results replacing the failed
verification. If a moderator approves the 22 October package first, the job sends
that version no more, and the credit reaches Chocolatey only with the next
release. The package is listed, and installs with `choco install
hmailserver-progressiverobot`, once a moderator has passed it. Every new version
waits for moderation the same way.

**winget: not submitted.** The manifests are rendered and validated by hand for
a release, from the release's own signed installer. The GitHub workflow
`package-managers.yml`, which was to do it for every release, arrived on
15 September 2026 (`6fc7ae91c`), after the `v6.3.3` tag, so no release ran it,
and it was not ported to GitLab (below). winget's community repository takes
pull requests on GitHub alone, and the project's submissions there are paused,
so `winget install ProgressiveRobot.hMailServer` does not work yet. This
document is the walk for that first submission and for everything after it.

**A winget package of the original is on its way.** Pull request 440546 to
`microsoft/winget-pkgs`, which a community packager opened on 24 September 2026,
adds the original as `hMailServer.hMailServer` 5.7.1; it had passed validation
and was waiting for a moderator on 21 October 2026. Once it is merged, `winget
install hMailServer` installs the original, not this project. And **both
installers register under the same uninstall entry, `hMailServer_is1`**: the
original's Inno installer has `AppName=hMailServer` and no `AppId`, which Inno
then takes from the name (`section_setup.iss` of its 5.7.0 builds, whose tags
this repository carries), and this project's pins `AppId=hMailServer`, so that
it upgrades an existing installation in place. So winget may take an installed
original 5.7 for this package and offer 6.3.7 as its upgrade, or take this
project's installation for the original's - and this project's database upgrade
has no step from the original's 5.7 releases, whose database versions are 5709
and 5711 (its chain runs from 5708 to 6001), so 6.3.7 is no upgrade of them. An
installation of the original up to 5.6 upgrades in place. A submission of `ProgressiveRobot.hMailServer` has to say so in
its pull request and ask the moderators how they want it handled
([The first submission, by hand](#the-first-submission-by-hand)).

**The identifiers.** On winget, `winget search hmailserver` found nothing on 15
September 2026, so the fork takes `ProgressiveRobot.hMailServer`, with the
moniker `hmailserver-progressiverobot`. On Chocolatey the identifier
`hmailserver` is the original's: <https://community.chocolatey.org/packages/hmailserver>
is hMailServer 5.6.8.2574, approved by a moderator on 4 February 2022, last
updated on 15 January 2022, with `ferventcoder`, `gep13` and `chtof` as its
maintainers, and **unlisted**: "This package is unlisted and hidden from package
listings" (read on 28 September 2026). An unlisted package is hidden from
search and installs only with its exact version named:
`choco install hmailserver` finds nothing ("The package was not found with the
source(s) listed"), and `choco install hmailserver --version 5.6.8.2574`
installs that package, the original's 5.6.8 (Chocolatey's CLI 2.4.3, run with
`--noop` on 21 October 2026).
Until the original was released again the project could have asked its
maintainers to add the project's account and published under it. The owner chose
`hmailserver-progressiverobot` instead, and the project does not publish under
`hmailserver`: with the original developed again, a package of the fork under
the original's id would have its users take the fork for the original's next
version. Chocolatey's packaging guidance asks that software already packaged not
be packaged again, so a moderator may ask why the fork has an id of its own; its
title and description answer that it is a fork, with releases of its own.
`HM_CHOCOLATEY_ID` is `hmailserver-progressiverobot` (set on 20 October 2026), and
`build/make-package-manifests.ps1 -ChocolateyId` and `chocolatey-publish` take
that id when none is given; the renderer still accepts `hmailserver` when it is
asked for by name, for a render by hand, and `chocolatey-publish` refuses it.

**The Chocolatey package is published by CI**, from `master`, by the jobs
`chocolatey-release` and `chocolatey-publish` ([Channels.md](Channels.md),
"Chocolatey"): the release's installer verified against its Sigstore bundle, the
package rendered from those bytes and packed on bench150, and pushed with
`CHOCO_API_KEY`, which has been set since 20 October 2026. winget stays by hand,
and paused.

What a user will type
---------------------

```powershell
winget install ProgressiveRobot.hMailServer      # not yet submitted
winget upgrade ProgressiveRobot.hMailServer
```

```powershell
choco install hmailserver-progressiverobot       # once moderation has approved it
choco upgrade hmailserver-progressiverobot
```

`winget install hMailServer`, once the original's package is merged, installs
the original hMailServer, not this project; `choco install hmailserver` finds
nothing, and `choco install hmailserver --version 5.6.8.2574` installs the
original's 5.6.8.

Both run the same Authenticode-signed installer the release page offers, with
the same switches the installer smoke test uses. Both install the server, the
Control Panel and the setup tools, and configure the embedded SQL Server Compact
database.

**An unattended install leaves the administrator password unset.** The wizard
page that asks for one is skipped, and neither package invents a password. Set
one before the server is reachable from anywhere but the machine itself: open
the Control Panel from the Start menu. `hmailserver --set-admin-password` is the
Linux server's command; `hMailServer.exe` on Windows has no such option.

Uninstalling (`winget uninstall`, `choco uninstall`) runs the installer's own
uninstaller. It does not delete the message store, the database or
`hMailServer.ini`: an administrator removing a package should not lose the mail.

What is in the repository
-------------------------

| Path | What it is |
|---|---|
| `hmailserver/installation/winget/*.yaml.in` | The three winget manifests — version, installer, `en-US` locale — as templates. Lines opening with `#~` are the template's own commentary and are dropped when rendered. |
| `hmailserver/installation/chocolatey/hmailserver.nuspec.in` | The Chocolatey package metadata, as a template. |
| `hmailserver/installation/chocolatey/tools/chocolateyinstall.ps1.in` | Downloads the release installer, checks it against the SHA-256, runs it unattended. |
| `hmailserver/installation/chocolatey/tools/chocolateyuninstall.ps1` | Runs the Inno uninstaller found through the `hMailServer_is1` registry key. Not a template: nothing in it changes between versions. |
| `hmailserver/source/Server/platform/packaging/hmailserver.png` | The fork's icon, which the Chocolatey package's `iconUrl` names by its raw address on `master` (below). |
| `build/make-package-manifests.ps1` | Renders all of the above for a version, computing the hash from the actual installer; `-ChocolateyId` is `hmailserver` or `hmailserver-progressiverobot`. |
| `build/ci/channels/chocolatey-publish.ps1` | The `chocolatey-publish` job ([Channels.md](Channels.md)): the verified installer rendered, packed with `choco pack` and pushed. |
| `build/check-package-manifests.py` | Validates the rendered manifests against Microsoft's published JSON schemas and cross-checks them against the Chocolatey package. |
| `package-managers.yml` (a GitHub workflow, in the git history at `faca023d4`) | Was to run the two on every published release, keep the result as an artifact, and submit when armed; it arrived after the `v6.3.3` tag, so no release ran it. It was not ported to GitLab ([ContinuousIntegration.md](ContinuousIntegration.md), "From GitHub Actions to GitLab CI"), so the two are run by hand. |

Where the values come from
--------------------------

Nothing in the manifests is a guess; every identity field is the installer's
own, from `hmailserver/installation/section_setup.iss` and `section_setup_64.iss`:

| Manifest field | Installer directive | Why it matters |
|---|---|---|
| `MinimumOSVersion: 10.0.14393.0` | `MinVersion=10.0.14393` | Windows 10 1607, the floor of the .NET 10 Desktop Runtime the Control Panel needs. |
| `ProductCode: hMailServer_is1` | `AppId=hMailServer` | Inno's uninstall key is `<AppId>_is1`. This is the only thing that lets winget recognise an installation it did not make — without it `winget upgrade` offers to install a second copy beside the first. |
| `DisplayVersion` | `AppVersion=<version>` | What `winget upgrade` compares against `PackageVersion`. |
| `DisplayName: hMailServer <version>-x64` | `AppVerName` | The name in Apps and Features. |
| `ElevationRequirement: elevatesSelf` | `PrivilegesRequired=admin` | The installer asks for elevation itself. |
| `InstallerType: inno` | Inno Setup | Gives winget the right default switches; ours are declared anyway. |

**The icon.** The nuspec's `iconUrl` is
<https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/hmailserver.png>,
the fork's own icon: the Control Panel's (`hmailserver/source/Tools/ControlPanel/app.ico`),
whose 256 by 256 frame is kept as that PNG byte for byte, and which the Helm chart,
the container image and the snap show too ([ContainerImage.md](ContainerImage.md),
[Snap.md](Snap.md)). It is the file on `master`, not at the version's tag: a
release's package is rendered from `master`'s templates, and the tags of 6.3.7 and
earlier have no such file. GitLab serves it as `image/png`, inline. Chocolatey's
automated validation of both of 6.3.7's packages, at 00:19 UTC on 21 October 2026
and at 01:39 UTC on 22 October, gave as a guideline (which does not hold a package
back) "The iconUrl should be added if there is one"; any package `chocolatey-publish`
renders from `master` once the icon is there carries it, a version sent again among
them.
`build/check-package-texts.py` holds the PNG to the `.ico`'s frame and every
listing to that one address, and `build/make-package-manifests.ps1` and
`build/check-package-manifests.py` hold the rendered `iconUrl` to it; the latter,
without `--offline`, also asks that the address answer as `image/png`.

The silent switches are `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.
`/SUPPRESSMSGBOXES` is not optional: without it a database-setup failure shows a
message box, silent mode answers it with the default button, and a broken
install reports success.

**A failed install says why only in the installer's log**, the message boxes being
suppressed - a failed database step, or, from the release after 6.3.7, the
installer's refusal of an installation of the original hMailServer's 5.7, which it
makes before it changes anything (Upgrading.md, "Where you can upgrade from"). Inno
Setup writes a log only with `/LOG`. The winget installer manifest declares `Log:
/LOG="<LOGPATH>"`, which `winget install --log <file>` fills. The Chocolatey
install script, in every package rendered from the templates in
`hmailserver/installation/chocolatey` - `chocolatey-publish` renders the latest
release's from `master`, 6.3.7's resubmission among them - passes
`/LOG="%TEMP%\<package id>.<version>.install.log"` and names that log when the
install fails, and shows the refusal's words, which setup from the release after
6.3.7 writes, with or without a log, to `%TEMP%\hMailServer-setup-refused.txt`.
Both end with the installer's exit code 1 for a refusal. The log records setup's
command line, so give the administrator password in `HM_ADMIN_PASSWORD`, set in
the elevated prompt, rather than with `/adminpassword` in `--override` or
`--install-arguments`.
`build/check-package-manifests.py` refuses a manifest or a package that has lost
either `/LOG`. `/TYPE=full` is deliberately **not** passed, unlike the
smoke test: on a first install Inno's default type is the full one anyway, and
on an upgrade Inno preselects the components already installed — forcing the
full type would quietly put the Control Panel back on a server where the
administrator had left it out.

Making the files for a release
------------------------------

Up to 6.3.3 the GitHub workflow did this on `release: published`. Since then it
is done by hand, once the release is published:

```powershell
# From the release that has been published (downloads the asset and hashes it):
build\make-package-manifests.ps1 -Version 6.3.3 -ReleaseDate 2026-09-15

# Or from a local copy of the SIGNED installer:
build\make-package-manifests.ps1 -Version 6.3.3 -ReleaseDate 2026-09-15 `
    -InstallerPath C:\...\hMailServer-6.3.3-x64.exe -RequireSignedInstaller

python build\check-package-manifests.py logs\package-manifests --release-date 2026-09-15
winget validate --manifest logs\package-manifests\winget\manifests\p\ProgressiveRobot\hMailServer\6.3.3
choco pack logs\package-manifests\chocolatey\hmailserver.nuspec --outputdirectory logs\package-manifests\chocolatey
```

`-ReleaseDate` is the day the release was published, in UTC, which the winget
installer manifest carries as `ReleaseDate`. Left out, it is read from the
GitLab release's `released_at` (the script stops if it cannot be read), and it is
never the day the script runs: the job that published 6.3.7's Chocolatey package,
at 23:47 UTC on 20 October 2026, was given no date and rendered `2026-10-21`, the
day on its machine's clock, for a release of 17 October. The `chocolatey-publish`
job now passes the date `chocolatey-release` read from the same release
([Channels.md](Channels.md), "Chocolatey"), and `check-package-manifests.py
--release-date <day>` refuses a manifest dated otherwise.

**Use the installer the release attached, not the one the build produced.** They
are different files: the release is Authenticode-signed after the build, and
signing changes the bytes and therefore the hash. For 6.3.3 the built installer
hashes to `45A9D7F1…` and the published one to `3188BE2F…`; a manifest carrying
the first would fail for every user with a hash mismatch.

The first submission, by hand
-----------------------------

winget:

1. Fork `microsoft/winget-pkgs` under the account that will submit.
2. Make a classic personal access token with `public_repo` scope.
3. Run the render above, then:
   ```powershell
   wingetcreate submit --token <token> `
       logs\package-manifests\winget\manifests\p\ProgressiveRobot\hMailServer\6.3.3
   ```
   `wingetcreate` is a single executable from <https://aka.ms/wingetcreate/latest>.
   It opens a pull request against `winget-pkgs` from the fork.
4. In the pull request, say that this is Progressive Robot's fork of
   hMailServer, not the original, and that its installer registers under the
   same uninstall entry as the original's, `hMailServer_is1` (the
   `ProductCode` of the installer manifest), and ask the moderators how they want
   that handled beside the original's `hMailServer.hMailServer`.
5. Their pipeline installs the package on a sandbox VM and a moderator reviews a
   new identifier. Expect questions the first time; answer them on the pull
   request.

Chocolatey (what `chocolatey-publish` does once `CHOCO_API_KEY` is set, except
that it pushes with an HTTP request carrying the key in a header rather than on
`choco push`'s command line):

1. The identifier is `hmailserver-progressiverobot` (above). `hmailserver` is
   the original's unlisted package, and a push under it is refused to an account
   that is not one of its maintainers.
2. Make an account on <https://community.chocolatey.org> and take the API key
   from the account page.
3. `choco push hmailserver-progressiverobot.6.3.3.nupkg --source https://push.chocolatey.org/ --api-key <key>`
4. A new package goes through moderation — automated checks first (the install
   and uninstall are run on a clean machine), then a human. The install script
   downloads a 76 MB installer, so allow for the verifier taking its time. A
   version that a moderator asks to have changed is pushed again as the same
   version, which takes the waiting package's place.

Arming the automatic submission
-------------------------------

*History: this is how the GitHub workflow `package-managers.yml` (in the git
history at `faca023d4`) was to submit each release by itself. No release ran
it, since it arrived after the `v6.3.3` tag; GitHub suspended the project's
account on 18 September 2026, and no GitLab job replaces it for winget, whose
submissions are made by hand as above; for Chocolatey, `chocolatey-publish` does
(below).*

Once the first version of each was accepted, every release could submit
itself. The workflow read, from the repository's settings:

| Kind | Name | Value |
|---|---|---|
| Variable | `PACKAGE_MANAGER_SUBMIT` | `true` |
| Secret | `WINGET_TOKEN` | The classic PAT with `public_repo` on the fork of `winget-pkgs` |
| Secret | `CHOCO_API_KEY` | The Chocolatey community feed key |

With the variable unset, or a secret missing, the workflow still rendered,
validated and kept the files — it just said in the log that it submitted
nothing. That was the deliberate default, and it holds for whatever replaces
it: no release should open a pull request against somebody else's repository by
surprise.

For Chocolatey, `chocolatey-publish` replaces it ([Channels.md](Channels.md)), with
the same default: it submits nothing until `CHOCO_API_KEY` is set, and then only a
release the update feed lists, whose installer it has verified. Its first package
was 6.3.7, pushed on 20 October 2026; no earlier release of this project is on
Chocolatey, under either id (the community repository's feed, read on 21 October
2026).

apt and dnf
-----------

The project keeps a signed apt repository and signed dnf repositories on the
same site as the update feed, https://updates.progressiverobot.com. `apt
upgrade` and `dnf upgrade` then bring each new release, like any other
package on the machine.

**Not live yet.** The job that makes them is written and was tested on linix
with a throwaway key and dummy packages ([What was tried](#what-was-tried)). It
publishes nothing until the owner has made the signing key and set it
([The signing key](#the-signing-key)). The site answers HTTPS with its own
certificate, which apt and dnf require, since 26 September 2026 (issue #30).

### What a user will type: Debian and Ubuntu

```sh
sudo install -d -m 0755 /etc/apt/keyrings
sudo curl -fsSLo /etc/apt/keyrings/hmailserver.asc https://updates.progressiverobot.com/hmailserver-repository-key.asc
gpg --show-keys /etc/apt/keyrings/hmailserver.asc   # compare the fingerprint with the one below
echo "deb [signed-by=/etc/apt/keyrings/hmailserver.asc] https://updates.progressiverobot.com/apt $(. /etc/os-release && echo "$ID$VERSION_ID") main" \
  | sudo tee /etc/apt/sources.list.d/hmailserver.list
sudo apt update
sudo apt install hmailserver
```

The key's fingerprint is **`(not made yet)`**. The owner writes it here when
the key is committed. A minimal image may lack `curl` (`sudo apt install curl
ca-certificates`) or `gpg`. The `gpg --show-keys` line is only for reading the
fingerprint: apt reads the armoured `.asc` keyring as it is.

The suite is the release's `$ID$VERSION_ID` from `/etc/os-release`, so the one
line fits every release the project builds for:

| Release | Suite | Its packages |
|---|---|---|
| Ubuntu 26.04 | `ubuntu26.04` | `hmailserver_<v>_<arch>.deb`, the generic name, from 6.3.4 on |
| Ubuntu 24.04 | `ubuntu24.04` | `hmailserver_<v>-1~ubuntu24.04_<arch>.deb` |
| Debian 13 | `debian13` | `hmailserver_<v>-1~debian13_<arch>.deb` |
| Debian 12 | `debian12` | `hmailserver_<v>-1~debian12_<arch>.deb` |

A suite exists once a stable release carries its packages. The package jobs
make the suffixed names from the release after 6.3.4
([PackagesDeb.md](PackagesDeb.md)), and 6.3.6 is the first release that carries
them, for both architectures (`build/ci/release-chain.py`'s `gathered_builds`);
until it is published only `ubuntu26.04` exists, with 6.3.4's x86-64 `.deb`. On
a release with no suite, `apt update` answers 404 for its `InRelease`.

### What a user will type: Fedora and Enterprise Linux

```sh
sudo curl -fsSLo /etc/yum.repos.d/hmailserver.repo https://updates.progressiverobot.com/rpm/fc44/hmailserver.repo
sudo dnf install hmailserver
```

Replace `fc44` with the release's directory. Each is named after the dist tag
in the `.rpm`'s own name (`hmailserver-<v>-1.el9.x86_64.rpm` is in `rpm/el9/`):
`fc44`, `fc43`, `el10` and `el9`. The first `dnf` command that reads the
repository asks to import its key, and shows the fingerprint to compare.

The generic `hmailserver-<v>-1.<arch>.rpm` is in no repository. `linux-build`
makes it on Ubuntu 26.04, so its automatic `Requires` are the sonames and symbol
versions of that build, and `linux-package-rpm` installs it on Fedora 44 with
`--nodeps` (`build/ci/linux-package-install.sh`). On 26 September 2026 `dnf
install` of the `.rpm` `linux-build` made from `master` that day resolved its
dependencies on Fedora 43 and 44 and AlmaLinux 10, and refused it on AlmaLinux
9, whose glibc and libstdc++ are older than it asks for. It is still no
distribution's own build, so a
dnf repository exists once a stable release carries the per-distribution
`.rpm` files the package jobs make; 6.3.6 is the first, with `fc44`, `fc43`,
`el10` and `el9` for x86_64 and aarch64. A release that carries the generic `.rpm`
alone, as 6.3.4 does today (the README's download line), adds nothing to them.
openSUSE's packages (`.suse.lp160`, `.suse.tw`) are not served either: zypper
is not dnf, and nothing here has been tried with it. zypper takes the release
through the openSUSE Build Service instead, which builds it from the source
tarball and signs its own repository ([Channels.md](Channels.md), "openSUSE Build
Service").

The `.repo` file:

```ini
[hmailserver]
name=hMailServer (Progressive Robot's fork) (fc44, $basearch)
baseurl=https://updates.progressiverobot.com/rpm/fc44/$basearch/
enabled=1
repo_gpgcheck=1
gpgcheck=0
gpgkey=https://updates.progressiverobot.com/hmailserver-repository-key.asc
```

`gpgcheck=0` does not mean the packages go unchecked:

- `repo_gpgcheck=1` makes dnf verify `repomd.xml` against its signature;
- `repomd.xml` gives the SHA-256 of `primary.xml`, which gives the SHA-256 of
  every package;
- dnf refuses a package that does not match.

That is the same chain apt uses. The packages carry no RPM signature of their
own. They are the release's files, byte for byte, signed with Sigstore
beside them, and signing them with `rpmsign` would change them.

With Fedora's defaults, dnf **skips** a repository whose signature fails. It
prints `>>> repomd.xml GPG signature verification error: Bad PGP signature`,
installs nothing from that repository, and exits 0 from `makecache`. AlmaLinux
9's dnf stops with an error instead. Add `skip_if_unavailable=0` to the file to
make Fedora stop too. The cost is that an outage of the site then stops every
`dnf upgrade` on the machine.

### What the repositories hold

- **Stable releases only.** A pre-release never reaches either repository.
- **Every stable release from the first one each place takes**, unless the job
  is given `--keep N`.
- **Which file goes where comes from its name.** A suffixed name says its
  release. The generic `.deb` goes where `GENERIC` in
  `build/generate-package-repos.py` says (`ubuntu26.04`), and only from 6.3.4,
  for both architectures: the amd64 one is `linux-build`'s, and the arm64 one,
  which a release carries from 6.3.6, is `deb-ubuntu26.04-arm64`'s, built
  against Ubuntu 26.04's own arm64 packages and installed by apt in an arm64
  Ubuntu 26.04 container. The generic `.deb`s of 6.3.0 to 6.3.3 were built on
  GitHub's Ubuntu 24.04 runners, so they are left out rather than offered to
  Ubuntu 26.04. The generic `.rpm` goes nowhere (above): the x86_64 one is built
  on Ubuntu, and the aarch64 one is the same Fedora 44 build as the `.fc44` file,
  which is the one `fc44`'s repository takes once a release carries it. Were a release to carry both a generic
  `.deb` and a suffixed one for the same suite, the repository would take the
  suffixed one.
- **The site holds only the metadata.** Each package's path on the site answers
  a 302 to the file in the package registry: the same file the release page
  and the update feed name, with the size and SHA-256 the signed metadata gives.
  The site therefore stays a few kilobytes per suite, well under the size
  GitLab allows a Pages site.
- **The limit is `_redirects`.** GitLab Pages reads 1,000 rules and 64 KiB,
  with one rule per package file. A rule is about 185 bytes, so the 64 KiB
  comes first, at about 350 rules. A release with every `.deb` and `.rpm` the
  package jobs make - 6.3.6 is the first - adds sixteen (eight `.deb`s: four
  suites, two architectures; eight `.rpm`s: `fc44`, `fc43`, `el10` and `el9`,
  two architectures; openSUSE's go through OBS), 2,942 bytes, beside the feed's
  own download links, which are seventeen from 6.3.6 (the download page, and
  eight files with their bundles: the x64 and ARM64 installers, the four generic
  packages and the two AppImages). So 6.3.6 alone makes 33 rules in 5,700 bytes,
  and twenty-one releases of its shape fit; the twenty-second does not. Counted on
  28 September 2026, and with the ARM64 installer on 29 September
  ([UpdateFeed.md](UpdateFeed.md), "How much the site holds"), by
  `build/ci/release-chain.py`'s `check_budget`, which the `release` job
  runs before anything is signed and `release-publish` before anything is
  uploaded, over the live releases with the new one in them, so a release that
  would take the site past the limit is refused before it is made, naming
  `--keep`; it counts the repositories with the `--keep` this job is given,
  read from the tree the release is made from. This job fails too, and
  deploys nothing, before a rule would be lost; `--keep` in
  `.gitlab/ci/package-repos.yml` is then the answer
  (UpdateFeed.md, "How much the site holds", has the feed's own budget).

### How it is made

`.gitlab/ci/package-repos.yml` holds one job, `package-repos`, which runs in
every pipeline `pages` runs in and needs it:

1. `pages` renders the update feed into `public/`, as before.
2. `package-repos` takes that `public/`. It reads `releases.json` for the
   stable releases' `.deb` and `.rpm` files and downloads each one from the
   registry. Each download must match the size and SHA-256 the registry records.
   A cache keyed by the SHA-256 keeps later runs from downloading them again.
3. It builds each apt suite with `apt-ftparchive`: `Packages`, `Packages.gz`
   and `Release`, clear-signed into `InRelease` and detach-signed into
   `Release.gpg`.
4. It builds each dnf repository with `createrepo_c`, and signs `repomd.xml`
   into `repomd.xml.asc`.
5. It adds the committed public key and the `_redirects` rules.
6. It reads everything back as a client would: the signatures by the
   committed key alone, every hash, and every rule going to the right file.
   Then it runs the feed's `--serve-check` again.
7. GitLab Pages deploys the result.

The `pages` job no longer deploys anything itself. The include turns its
deployment off with `pages: false`, a key GitLab merges into the job of that
name. If both deployed, the feed alone would be live between the two.

The tools are Debian 13's own `apt-utils`, `dpkg-dev`, `createrepo-c` and
`gnupg`. They run in `debian:13` pinned by digest, on linix, server2 or s1-linux like
`pages`.

**Without the key, the feed alone.** If `HM_REPO_SIGNING_KEY` is not set, or
the public key is not committed, the job prints what is missing, adds nothing
and passes, so the site is the update feed alone. A protected variable reaches
only a protected branch or tag, and `pages` runs only on those. Any other
failure fails the job, and GitLab deploys nothing, so the site keeps what it
had. Such failures include a key that is not the committed one, a package whose
size or hash is not the registry's, and more rules than Pages reads.

The same script by hand, from the repository root:

```sh
python3 build/generate-package-repos.py --self-test
python3 build/generate-package-repos.py --site public            # after generate-update-feed.py --output public
python3 build/generate-package-repos.py --check public
python3 build/generate-package-repos.py --serve public --port 8080   # the site as Pages serves it, _redirects included
```

### The signing key

The owner makes it, on a machine of their own and not on GitLab. The primary
key certifies only, and stays offline. A signing subkey is the only secret
GitLab holds, so if it leaks it can be revoked and replaced without users
changing the key they trust. With GnuPG 2.2 or later:

```sh
# The primary key: certify only, no expiry. gpg asks for a passphrase.
gpg --quick-generate-key "hMailServer package repository <chris.holloway@progressiverobot.com>" ed25519 cert never
FPR=$(gpg --list-keys --with-colons "hMailServer package repository" | awk -F: '/^fpr/ {print $10; exit}')

# The signing subkey, for five years.
gpg --quick-add-key "$FPR" ed25519 sign 5y
SUB=$(gpg --list-keys --with-colons "$FPR" | awk -F: '/^sub/ {getline; print $10; exit}')

# The public key, which users trust: commit it.
gpg --armor --export "$FPR" > .gitlab/package-repository-key.asc
gpg --fingerprint "$FPR"     # write this fingerprint into this document, above

# Offline, with the revocation certificate gpg wrote under ~/.gnupg/openpgp-revocs.d/:
gpg --armor --export-secret-keys "$FPR" > hmailserver-repository-primary.asc

# The subkey alone, for GitLab: the primary's secret is left out ("sec#").
gpg --export-secret-subkeys "$SUB!" | base64 -w0 > repo-signing-subkey.b64
```

Then set the CI/CD variables, under Settings, CI/CD, Variables. Each is
**Protected** and **Masked and hidden**:

- `HM_REPO_SIGNING_KEY`: the content of `repo-signing-subkey.b64`. The job also
  takes a File variable, or ASCII armour, but a masked value must be one line,
  which base64 is.
- `HM_REPO_SIGNING_PASSPHRASE`: the key's passphrase. Masking needs at least
  eight characters from the base64 alphabet (and `@:.~`). Choose one like that
  (`openssl rand -base64 24`), or change the passphrase before the export
  (`gpg --passwd "$FPR"`).

The same from a shell, with `glab` signed in as the owner:

```sh
glab variable set HM_REPO_SIGNING_KEY --protected --masked < repo-signing-subkey.b64
read -rs PASS && printf '%s' "$PASS" | glab variable set HM_REPO_SIGNING_PASSPHRASE --protected --masked
rm repo-signing-subkey.b64
```

The job refuses a key that does not verify as the committed one. It imports the
committed public key and the variable's secret into a GnuPG home of its own, and
requires a usable signing key under the committed fingerprint. It then signs a
probe and verifies it with the public key alone. It notes if the variable holds
the primary key's secret too.

**Renewing.** From 180 days before the subkey expires, the job warns in its log.
Extend it with the primary key (`gpg --quick-set-expire "$FPR" 5y "$SUB"`),
commit the exported public key again, and ask users to fetch the key file again:
neither apt nor dnf refreshes a key by itself. To replace a leaked subkey, revoke
it with the primary, add a new one, commit the public key, and set the variable
again.

**Before it is announced:**

1. Make the key, commit `.gitlab/package-repository-key.asc`, and write the
   fingerprint above. Add the file to `.gitlab/CODEOWNERS` with the other
   supply-chain files.
2. Set the two variables.
3. Make sure https://updates.progressiverobot.com answers with its own
   certificate.
4. Run a pipeline on `master`. In `package-repos`' log, look for `signing as`
   and the fingerprint.
5. Run the commands above against the live site on a real Debian, Ubuntu,
   Fedora and Enterprise Linux machine. [What was tried](#what-was-tried) used
   plain HTTP and an emulation of Pages, not GitLab Pages itself.

### What was tried

These ran on linix (26 September 2026), and again after a check of the work
had the generic `.rpm` kept out of every repository, with `.fc44` packages
added to the inputs:

- **The builder:** Debian 13 by digest, with the four tools installed from
  Debian (apt-utils 3.0.3, createrepo-c 1.2.0, gnupg 2.4.7, python3 3.13.5).
- **The inputs:**
  - a throwaway key, made with the commands above;
  - dummy packages built with `dpkg-deb` and `rpmbuild`: generic and
    `~debian13` `.deb`s, and generic, `.fc44` and `.el9` `.rpm`s, for 6.3.3,
    6.3.5 and 6.3.6;
  - a pre-release;
  - a `releases.json` pointing at a stand-in registry, served by `python3 -m
    http.server` on another port.
- **The site** was served by `--serve`, which answers as GitLab Pages does: a
  file first, then an exact `_redirects` rule, matched on the decoded path.

What happened:

- **The script's self-test passed with the tools there.** That includes a whole
  apt repository built, read back, and caught three times: when `Packages` was
  changed, when a rule sent a package elsewhere, and when another key was set.
- **Without `HM_REPO_SIGNING_KEY`,** the job printed its refusal, exited 0 and
  left the site as it was.
- **With the key:**
  - two apt suites (`debian13`, `ubuntu26.04`) and two dnf repositories (`el9`,
    `fc44`) were built;
  - the pre-release, 6.3.3's generic `.deb` and every generic `.rpm` were
    left out, each said;
  - `--check` found no problem.
- **Debian 13 and Ubuntu 26.04, clean containers** (Debian 13's apt verified
  with `sqv`, as its refusal below names):
  - the sources line above, with the site's address changed;
  - `apt-get update`;
  - an install of the oldest version, then `apt-get upgrade` to the newest.
- **Fedora 44 (dnf5) and AlmaLinux 9 (dnf 4), clean containers:**
  - the published `.repo` file, with the site's address changed;
  - the key imported from `gpgkey=`;
  - `dnf install hmailserver-6.3.5`, then `dnf upgrade` to 6.3.6.
- **Every package went through the redirect.** The site's log shows each `.deb`
  and `.rpm` answered with a 302, and the registry's log shows each one
  fetched. apt asks for `~` as `%7e`, and the rule, written with `~`, matched
  once decoded. GitLab Pages matches rules on the decoded path too
  (gitlab-pages, `internal/redirects/matching.go`).
- **Refused, as they should be:**
  - apt, with another key in `signed-by`: "is not signed", on both;
  - apt, with a `Packages` file changed after signing: "File has unexpected
    size";
  - dnf, with `repomd.xml` changed after signing: nothing installed, on both.
    Fedora skipped the repository (see above); AlmaLinux 9 stopped with "Bad
    GPG signature", and so did Fedora with `skip_if_unavailable=0` in the
    `.repo` (`makecache` exit 1 on both).
- **GitLab's CI lint** (the API, which creates nothing) accepted the merged
  configuration: `.gitlab-ci.yml` with `pages: false` in `pages`, plus
  `package-repos`. Its dry run of a `master` pipeline created both jobs.

Not tried:

- the job on GitLab's runners, or a deployment to GitLab Pages;
- a 302 from real GitLab Pages to the registry. gitlab-pages accepts an absolute
  `http(s)` target for 301 and 302 (`internal/redirects/validations.go`), and
  the registry answers a public project's file with 200, not a further redirect;
- HTTPS end to end;
- arm64 and aarch64;
- Debian 12 and Ubuntu 24.04, whose packages do not exist yet.

What this does not do
---------------------

* **No Arch package here.** The `.deb` and `.rpm` have the apt and dnf
  repositories above; the AppImages were attached to the releases 6.3.0 to
  6.3.3, and again from 6.3.6, but not to 6.3.4 or 6.3.5 ([PackagesAppImage.md](PackagesAppImage.md));
  the Arch package is the AUR's `hmailserver`, which `aur-publish` publishes from
  the release's source tarball ([Channels.md](Channels.md)).
* **No `winget install` of a pre-release.** The workflow refused a tag that was
  not three numbers, and a pre-release is not submitted by hand either, because package managers should not hand an alpha to
  somebody typing `winget install ProgressiveRobot.hMailServer`.
* **No unattended configuration.** Neither package sets an administrator
  password, adds a domain, or opens a firewall port. Installing a mail server is
  one command; configuring one is not, and a package that pretended otherwise
  would be a security defect rather than a convenience.
