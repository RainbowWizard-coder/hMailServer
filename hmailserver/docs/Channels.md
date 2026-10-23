Release channels
================

Where a release of hMailServer is published besides its own release page, and how.
Each channel has a job of its own in `.gitlab/ci/channels.yml`, started by hand on
`master`, which publishes a signed release there once the channel's account exists.
The Windows installer's other package managers, winget and Chocolatey's history, and
the project's own apt and dnf repositories are in
[PackageManagers.md](PackageManagers.md); the command-line clients' PyPI and
PowerShell Gallery packages in [CommandLinePackages.md](CommandLinePackages.md).
Two channels have no job here because nothing is published to them: the Nix flake
in the repository itself ([Nix.md](Nix.md)), and Alpine Linux, whose package is a
merge request to aports built from the APKBUILD in the tree ([Alpine.md](Alpine.md)).

**Where each channel stands, read on 21 October 2026, Chocolatey's on 23 October.** Three build services carry
6.3.7, each from its job's run on `master` that day ([What was tried](#what-was-tried)):
OBS's home project `home:progressiverobot` publishes it for openSUSE Leap 16.0 (x86_64
and aarch64) and Tumbleweed (x86_64), in repositories OBS signs; Copr's
`progressiverobot/hmailserver` built it for Fedora 44 and 43 and EPEL 10, each on x86_64
and aarch64, and serves it; and the PPA `ppa:hmailserver/hmailserver` publishes it for
Ubuntu 24.04 and 26.04, amd64 and arm64. Docker Hub's `progressiverobot/hmailserver` has
it too, copied by hand that day after `registry-mirror`, written for an image of one
platform, refused the release's image index; the job now verifies and copies an index
([Container registries](#container-registries-docker-hub-quayio-amazon-ecr-public)).
Chocolatey's job pushed `hmailserver-progressiverobot` 6.3.7 on 20 October 2026, which
then waited for Chocolatey's moderation; a moderator approved it on 23 October 2026
([Chocolatey](#chocolatey)). Quay.io, ECR Public, the AUR and the Snap Store have
nothing: `registry-mirror` copies only to a registry whose credential is stored, and
`aur-publish` says "not configured" and passes, until the owner has made the channel's
account and a maintainer has stored its credential; the Snap Store's credential is
stored, and its first upload, of 6.3.7, failed ([Snap Store](#snap-store)). Every job
was first run on a laptop against a fixture release. No release before 6.3.6 can be
published to the AUR, the PPA, Copr or OBS: they build from the release's source tarball
([SourceTarball.md](SourceTarball.md)), and the Debian and RPM packaging inside it
([SourcePackages.md](SourcePackages.md)), which 6.3.6 is the first to carry.

| Channel | What a user types | Job | Credential (project CI/CD variable) |
|---|---|---|---|
| Docker Hub | `docker pull progressiverobot/hmailserver:<version>` | `registry-mirror` | `DOCKERHUB_USERNAME`, `DOCKERHUB_TOKEN` |
| Quay.io | `docker pull quay.io/hmailserver/hmailserver:<version>` | `registry-mirror` | `QUAY_ROBOT_USER`, `QUAY_ROBOT_TOKEN` |
| Amazon ECR Public (optional) | `docker pull public.ecr.aws/<alias>/hmailserver:<version>` | `registry-mirror` | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `ECR_PUBLIC_REPOSITORY` |
| Arch User Repository | `git clone https://aur.archlinux.org/hmailserver.git && cd hmailserver && makepkg -si` | `aur-publish` | `AUR_SSH_KEY` |
| Ubuntu PPA | `sudo add-apt-repository ppa:hmailserver/hmailserver && sudo apt install hmailserver` | `ppa-upload` | `LAUNCHPAD_SIGNING_KEY` |
| Fedora Copr | `sudo dnf copr enable progressiverobot/hmailserver && sudo dnf install hmailserver` | `copr-build` | `COPR_WEBHOOK_URL` |
| openSUSE Build Service | `sudo zypper addrepo https://download.opensuse.org/repositories/home:/progressiverobot/16.0/home:progressiverobot.repo` (`openSUSE_Tumbleweed` in place of `16.0` for Tumbleweed), then `sudo zypper install hmailserver` | `obs-publish` | `OBS_RUNSERVICE_TOKEN`, `OBS_PROJECT` |
| Chocolatey | `choco install hmailserver-progressiverobot` | `chocolatey-release`, then `chocolatey-publish` | `CHOCO_API_KEY`, `HM_CHOCOLATEY_ID` |
| Snap Store | `sudo snap install hmailserver --candidate` | `snap-upload` | `SNAPCRAFT_STORE_CREDENTIALS` |

How a channel job publishes
---------------------------

Every job below works the same way (`build/ci/channels/release-files.py`, which each
loads):

1. **Only on `master`, only by hand.** The jobs are created in a protected `master`
   branch pipeline and nowhere else: never for a tag, a merge request, a schedule or
   a downstream pipeline. Each is manual and allowed to fail, so a
   `master` pipeline is never held for a channel nobody has set up.
2. **A release, never the commit.** The release is the one the project's CI/CD
   variable `HM_CHANNEL_VERSION` names (`6.3.6`), or else the latest release in the
   update feed (`https://updates.progressiverobot.com/releases/latest`). Either way
   the feed must list it as published, not a draft and not a pre-release. This is
   the rule of `build/ci/cli-packages.py`, whose `pypi-publish` and
   `psgallery-publish` use it too. A version is never given when a job is started:
   the project refuses pipeline variables
   (`ci_pipeline_variables_minimum_override_role` is `no_one_allowed`). So an account
   that arrives weeks after a release still publishes that release, from `master`,
   with no new release made for it.
3. **Verified before anything is sent.** The job reads the GitLab release
   `v<version>` anonymously, and takes each file it publishes from the generic
   package registry, where `build/ci/release-chain.py` put it, with its
   `<file>.cosign.bundle`. Each file must verify with `cosign verify-blob` as signed
   by this project's pipeline for that tag:
   `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
   or the same at `Progressiverobot/hmailserver` for a release signed before the
   project moved on 29 September 2026 (6.3.5), with the issuer `https://gitlab.com`. At either path the
   certificate must also carry the project's id, 86730559 (Fulcio's source repository
   identifier), and any id but that one is refused: the rule every server from 6.3.6
   applies to a live update (`SigstoreVerifier.cpp`; 6.3.5's certificate records it).
   A file that does not
   verify stops the job before it logs in anywhere, and the log says what cosign
   said. The id is read from the certificate's extensions, not from wherever its
   bytes occur: a public key is bytes its holder chooses. The image is verified in
   the registry the same way (below); the snap, which is no file of the release, is
   held to the release's verified tarball or its tag instead (below).
4. **Dormant until set up.** Without its credential a job says `not configured`,
   names what the owner and a maintainer do, and exits 0 - before it reads the feed.
   A credential that is half there (a user without its token) fails the job.
5. **The tools from their own images.** `cosign` (2.6.5, the release chain's) and
   `regctl` (0.11.6) are static binaries the script takes out of their images, pinned
   by digest: `HM_COSIGN_IMAGE` in `.gitlab-ci.yml` (gcr.io) and `HM_REGCTL_IMAGE` in
   `.gitlab/ci/channels.yml` (Docker Hub). Every manifest and layer is held to its
   digest. Nothing is downloaded from GitHub, and every job here in a Docker image
   takes `.no-github` first (`.gitlab/ci/no-github.yml`): no runner it runs on keeps
   GitHub out for it.
6. **No secret on a command line.** Each credential is a protected, masked project
   variable. The job reads it from its environment and hands it on standard input
   (`regctl registry login --pass-stdin`, `gpg --import` of the PPA's OpenPGP key into
   a GnuPG home of the job's own), in a file of its own with mode 0600 (the AUR's SSH
   key, the PPA key's passphrase), in a request header (OBS, Chocolatey), or as the
   address the job calls (Copr's webhook, whose UUID is the secret); snapcraft reads
   its own from the environment. None is printed.
7. **Once is enough, and never backwards.** A job run again for a release its
   channel has already says so and passes: the AUR's PKGBUILD byte for byte, the
   PPA's version per Ubuntu release, a Chocolatey version the community repository
   lists (listed or unlisted), a snap the store's `edge` and `candidate` both hold.
   The registries are copied again harmlessly, since a copy by digest changes
   nothing. Two are not held to it: a Chocolatey version still waiting in moderation,
   which the repository's feed lists with the status `Submitted` (it answered so for
   `hmailserver-progressiverobot` 6.3.7 on 21 October 2026), is sent again by a
   second press, in the waiting package's place, as Chocolatey's moderation asks of a
   package changed during its review ("the exact same version"); and a second
   `copr-build` starts another Copr build of the same release. A release older than the channel's is
   refused by the AUR, the snap (per architecture, against what `edge` and
   `candidate` hold) and OBS (the latest release alone); on the PPA, Chocolatey and
   Copr an older version beside a newer one moves nobody back, since apt, choco and
   dnf take the highest; and the registries never move `:<major>.<minor>` or
   `:latest` back.

Each job has a GitLab environment of its own (`container-registries`, `aur`,
`launchpad`, `copr`, `obs`, `chocolatey`, `snap-store`), so each credential can be
scoped to its environment (Settings, CI/CD, Variables: "Environments"), and a resource
group, so two presses never publish at once. On GitLab Free there are no protected
environments: the scope keeps a credential from the jobs that declare another
environment or none, but any job of a protected pipeline may declare `snap-store` or
`aur` and be given it. What a credential may do is therefore limited where the
service lets it be - the Snap Store's to `edge` and `candidate` (below) - and the
protected refs it reaches are the maintainers' alone (ContinuousIntegration.md).

The channels
------------

### Container registries: Docker Hub, Quay.io, Amazon ECR Public

`registry-mirror` (`build/ci/channels/registry-mirror.py`) copies the release's
image from `registry.gitlab.com/hmailserver/hmailserver`, the only place the
release's pipeline pushes it ([ContainerImage.md](ContainerImage.md)).

- **What it verifies.** `:<version>` is resolved to its digest, which since 6.3.6
  is an image index over linux/amd64 and linux/arm64: the release's pipeline signs
  the index by its digest, and signs each platform's image and attests its
  CycloneDX SBOM to that image, attesting nothing to the index
  ([ContainerImage.md](ContainerImage.md), "Checking an image"). The index's
  manifest by that digest and cosign's signature manifest beside it
  (`sha256-<index digest>.sig`, the tag resolved once and the manifest then fetched
  by its digest and held to it, with its blobs) are written as cosign's own OCI
  layout (the one `cosign save` writes), and `cosign verify --local-image` verifies
  the index's signature there, as the tag's pipeline (the identities above). Then
  every entry of that same index manifest, read from the layout and not fetched
  again, must be an image manifest with its platform's operating system and
  architecture, no two of one platform: a nested index, an entry without a
  platform, a build's attestation manifest (`unknown/unknown`) or an empty index is
  refused. Each platform's image is verified in a layout of its own, its manifest
  held to the digest, size and media type the index gives it, with its signature
  and attestation manifests (`sha256-<platform digest>.sig` and `.att`):
  `cosign verify --local-image` of its signature and `cosign verify-attestation
  --local-image --type cyclonedx` of its SBOM, both as the identity the index
  verified as. A single-platform image, a manifest rather than an index at
  `:<version>`, is verified as each platform's image is. In every layout, with no
  second read of the registry, the project's id is required in every signing and
  every attesting certificate that names the identity, read from its extensions
  (`build/ci/sigstore-certificate.py`) in the same manifests: this project's, and
  one required at either path. A manifest or an index whose keys Go's JSON reader,
  cosign's and regctl's, could read otherwise - a field's name in another case or
  outside ASCII, a key twice, a key that is not one of the manifest's, the index's,
  a descriptor's or a platform's fields, an index at a signature's tag - is refused.
- **What it copies.** `regctl image copy` of the source by its digest, to
  `:<version>` always, and to `:<major>.<minor>` and `:latest` only where the
  source's own tag of that name is the same digest: an older release mirrored late
  never moves them back. The copy of an index carries every platform's image it
  names (regctl copies each entry of an index unless it is told which platforms).
  Then cosign's manifests, copied out of the layouts they were verified in: the
  index's `.sig` at `sha256-<index digest>.sig`, and each platform's `.sig` and
  `.att` at `sha256-<platform digest>.sig` and `.att` - the manifests that were
  verified, not whatever the source's tags answer by then, so not `--digest-tags`
  or `--referrers`, which would copy the source's as they stand, unverified (nothing
  is attached to the image as a referrer). A `.sig` or `.att` the destination
  already holds at those tags - from an earlier run, or put there by anyone else
  with push access - is replaced by the verified one, not added to. Each tag at the
  destination is held to its digest, and the index and every platform's image there
  are verified again the same way, anonymously, from the destination: regctl reads
  them with a configuration directory of its own that no login has been written to,
  so the check cannot run logged in, whatever the logout before it did (a failed
  logout is said in the log). `mirror.json`, the job's record, gives the index's
  digest and signature beside each platform's digest, signature and attestation.
- **The destinations**, each configured by its pair of variables:
  - Docker Hub: `docker.io/progressiverobot/hmailserver` (or `DOCKERHUB_REPOSITORY`,
    which must be on `docker.io`). A Personal account's public repository is free;
    an organisation `hmailserver` would cost Docker Team.
  - Quay.io: `quay.io/hmailserver/hmailserver` (or `QUAY_REPOSITORY`, on `quay.io`).
    Quay scans each pushed image with Clair.
  - Amazon ECR Public, only if wanted: `ECR_PUBLIC_REPOSITORY`
    (`public.ecr.aws/<alias>/hmailserver`) is required with the AWS key pair; the job
    asks ECR Public for a registry password with AWS Signature Version 4 (checked
    against AWS's own published vectors in the self-test).
- **The owner, once.** Docker Hub (done on 21 October 2026): sign in as
  `progressiverobot`, make the public repository `hmailserver` and a Read & Write
  access token. Quay.io: a Red Hat
  account, the organisation `hmailserver`, a **public** repository `hmailserver`, and
  a robot account with write access to it. ECR Public: an AWS account, the public
  repository, and an IAM user limited to `ecr-public:*` on it and
  `sts:GetServiceBearerToken`.

### The Arch User Repository

`aur-publish` (`build/ci/channels/aur-publish.py`, in Arch Linux's own image) pushes
the package `hmailserver` to `ssh://aur@aur.archlinux.org/hmailserver.git`. The name
was free on 28 September 2026.

- **What it verifies.** The release's `hmailserver-<version>.tar.gz`.
- **What it publishes.** `hmailserver/source/Server/platform/packaging/PKGBUILD` as
  `master` has it, rendered for the release: `pkgver` the release's, `sha256sums` the
  verified tarball's SHA-256 (the tree's own file says `SKIP`), and `pkgrel` 1 - or
  the AUR's `pkgrel` plus one when the same release's PKGBUILD has changed on
  `master` since it was pushed. `.SRCINFO` is made from it by `makepkg
  --printsrcinfo`, as an unprivileged user, and held to what was rendered. A release
  older than the AUR's is refused.
- **The package.** It builds the tarball with clang, without link-time optimisation
  (Arch's default full-LTO link of the server needs more than 6 GB), and with
  `-DHM_LIVE_UPDATE=OFF`, like every distribution's build ([LiveUpdateKinds.md](LiveUpdateKinds.md)): the update
  check names `makepkg -si from https://aur.archlinux.org/hmailserver.git` instead of
  installing anything itself. The user, the directories and the unit are declared
  for pacman's own hooks; the unit is not enabled.
- **SSH.** The key is written to a file of the job's own; git's ssh uses it alone,
  with `StrictHostKeyChecking=yes` and the AUR's three host keys from
  `build/ci/channels/aur.known_hosts`, which the script holds to the fingerprints
  the AUR publishes on its home page before it uses them.
- **The owner, once.** An AUR account, signed in. A maintainer makes an ed25519 key
  (`ssh-keygen -t ed25519 -N "" -f aur`), pastes `aur.pub` into the account's SSH
  public key, and stores `base64 -w0 aur` as `AUR_SSH_KEY`. The first push creates
  the package.

### Ubuntu PPA

`ppa-upload` (`build/ci/channels/ppa-upload.py`, in Debian 13) uploads the release's
Debian source package to `ppa:hmailserver/hmailserver`, which Launchpad builds for
each Ubuntu release: `noble` (24.04) and `resolute` (26.04) unless
`LAUNCHPAD_SERIES` names others.

- **What it verifies.** The release's source tarball.
- **What it uploads.** The packaging inside that tarball,
  `hmailserver/source/Server/platform/packaging/debian-src/debian`, never this
  checkout's: the tarball repacked by `mk-origtargz` with `debian/copyright`'s
  `Files-Excluded` into `hmailserver_<version>+ds.orig.tar.gz` (or, when an earlier
  upload of the version sent it, the PPA's own copy, held to the repack file by
  file, since Launchpad keeps one per version); then for each Ubuntu release the PPA
  does not have yet, a changelog entry and the version `<package
  version>~<release>1` (`6.3.6+ds-1~noble1`, which sorts before Ubuntu's own
  `6.3.6+ds-1` and after the release before it), `dpkg-buildpackage -S`, `debsign`
  with the key, and `dput` to `ppa.launchpad.net`, which takes an upload on its
  signature alone. While the PPA holds no orig of the version, every release's
  upload carries it (`-sa`), the same bytes each time; once it holds one, none does
  (`-sd`). Launchpad takes a file it already holds when its bytes are the same and
  refuses one whose bytes differ, and refuses an upload that names an orig it does
  not hold: so no upload rests on another having been taken first (until 6.3.6 the
  first release's upload alone carried it).
- **The owner, once.** On Launchpad: the team `~hmailserver` and its PPA
  `hmailserver`, with arm64 builds enabled. The maintainers make a dedicated upload
  key (`gpg --quick-generate-key "hMailServer PPA upload <...>" ed25519 sign 2y`); the
  owner registers its fingerprint on his Launchpad account (a member of the team).
  The key signs and does not encrypt, so Launchpad mails the confirmation link in the
  clear: the link, opened signed in as the owner, asks for the mail's paragraph
  clear-signed with the key, which the maintainers sign (`gpg --clearsign`) for him to
  paste. `gpg --export-secret-keys <fingerprint> | base64 -w0` is
  `LAUNCHPAD_SIGNING_KEY`; `LAUNCHPAD_SIGNING_PASSPHRASE` if the key has one.
  `LAUNCHPAD_PPA` (`<owner>/<name>`) if the PPA is not `hmailserver/hmailserver`.

### Fedora Copr

`copr-build` (`build/ci/channels/copr-build.py trigger`) starts a build of the Copr
package `hmailserver`, whose source is this repository's `master`, built with
`make_srpm`: Copr clones `master` and runs `.copr/Makefile`, which runs
`copr-build.py srpm` in Copr's own builder.

- **What it verifies, twice.** The job verifies the release's source tarball, and
  that the RPM spec inside it (`hmailserver/source/Server/platform/packaging/rpm/
  hmailserver.spec`) is at the release's version, before it calls the webhook. Then
  Copr's builder does it again, with the same cosign pin, rule and identities, and
  makes the source RPM from the verified tarball and the spec and `Source1` inside
  it - never from the clone. A push to `master` therefore changes nothing Copr
  builds; a release does.
- **The latest release alone.** Copr's builder has no `HM_CHANNEL_VERSION`, so it
  always takes the update feed's latest; the job refuses when the variable names
  another release, rather than verify one and start a build of the other. Copr
  keeps no record the job reads of what it built, so a second press starts a second
  build of the same release; the repository then holds that version once more.
- **The credential.** The package's custom webhook,
  `https://copr.fedorainfracloud.org/webhooks/custom/<id>/<uuid>/hmailserver/`, as
  `COPR_WEBHOOK_URL`: its UUID is the secret, and unlike a Copr API token (180 days)
  it does not expire. The job refuses any other host.
- **The owner, once.** A Fedora account, then the Copr project `hmailserver` with
  the chroots `fedora-44`, `fedora-43` and `epel-10`, each `x86_64` and `aarch64`
  (not `epel-9`: the spec builds against the distribution's Boost, 1.83 or newer, and
  EL9 has 1.75), and in it the package `hmailserver`: source type SCM, clone URL
  `https://gitlab.com/hmailserver/hmailserver.git`, committish `master`, build
  method `make_srpm`. The project's Integrations page gives the webhook.

### openSUSE Build Service

The OBS package `hmailserver` holds one file,
`hmailserver/source/Server/platform/packaging/obs/_service`. When it runs, OBS
downloads the latest release's source tarball through GitLab's permanent link
(`https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/
hmailserver.tar.gz`, no token: the project is public), takes the spec and its
`Source1` out of it, and fetches the spec's `Source0`, the same tarball under its
versioned name. OBS builds for Leap and Tumbleweed and publishes a repository it
signs with its own key, which is the zypper repository.

`obs-publish` (`build/ci/channels/obs-publish.py`) starts that service, and only for
a verified release: it verifies the release's tarball, checks the spec inside it,
downloads the permanent link and holds it to the verified SHA-256 - so a release
that is not the latest is refused - and then calls OBS's `runservice` trigger with
its token in a header.

What OBS builds is what it downloads itself when the service runs, a few moments
later, and OBS checks nothing but HTTPS: the job has held the permanent link to the
verified bytes when it looked, not when OBS looks. The two differ only if GitLab
serves other bytes in between - a newer release published in that window, or a
file replaced in the package registry - so OBS is the one channel whose build is
not tied to the verified bytes themselves; the AUR and Chocolatey pin the SHA-256,
Copr's builder verifies again, and the PPA uploads the verified file.

- **The owner, once.** An openSUSE account and its home project (or a project of
  its own), the package `hmailserver` with the tree's `_service` in it (`osc add` or
  the web page), the build targets openSUSE Leap 16.0 and Tumbleweed, and a token:
  `osc token --create --operation runservice <project> hmailserver`, stored as
  `OBS_RUNSERVICE_TOKEN`, with the project's name as `OBS_PROJECT`.

### Chocolatey

Two jobs, because `cosign` has no Windows build outside GitHub and `choco` runs on
Windows. Pressing `chocolatey-release` starts both.

- `chocolatey-release` (linix, server2 or s1-linux): `release-files.py verify
  "hMailServer-{version}-x64.exe"`, the installer verified as above, and
  `verification.json` handed on, with the release's `released_at` and its day
  (`release_date`), as `verify` writes them for every channel.
- `chocolatey-publish` (bench150, `build/ci/channels/chocolatey-publish.ps1`): the
  installer downloaded from the release and held to that SHA-256 and to a valid
  Authenticode signature; a version the community repository has, listed or not, is
  not sent again; `build/make-package-manifests.ps1` renders the package from those
  bytes, dated by the release (the `release_date` that `chocolatey-release` wrote
  into `verification.json` from the GitLab release's `released_at`, in UTC, and
  without which the job stops: never the day the job runs), `choco pack` packs it,
  and the `.nupkg` is pushed to
  `https://push.chocolatey.org/` with an HTTP PUT and the key in its
  `X-NuGet-ApiKey` header (not `choco push`, which takes the key on its command
  line). A new package, and every new version, then waits for Chocolatey's
  moderation before it is listed.
- **The id: `hmailserver-progressiverobot`.** `hmailserver` is the original
  hMailServer's package, 5.6.8.2574, last updated on 15 January 2022 and now unlisted
  (the page does not say since when), maintained by `ferventcoder`, `gep13` and
  `chtof`. With the original developed again, the owner chose an id of the fork's own,
  and the project does not publish under the original's ([PackageManagers.md](PackageManagers.md)
  says why). `HM_CHOCOLATEY_ID` holds it, and the job takes it when the variable is
  unset; it refuses `hmailserver`, the original's, if the variable names it, before
  it reads or makes anything.
- **Set up on 20 October 2026.** The owner made the account on community.chocolatey.org,
  and its API key is `CHOCO_API_KEY`. bench150 has Chocolatey's CLI
  (`https://community.chocolatey.org/install.ps1`), whose `C:\ProgramData\chocolatey\bin`
  its runner puts on the job's `PATH`; the job stops, saying so, where it is not.
- **The first package.** `chocolatey-publish` (job 17333239753, in `master`'s
  pipeline 2966261322) verified 6.3.7's installer (Authenticode Valid, "Progressive
  Robot Ltd"), packed `hmailserver-progressiverobot.6.3.7.nupkg` and pushed it at
  23:47 UTC on 20 October 2026 (HTTP 201). That package was titled "hMailServer"
  alone, its authors and copyright named Progressive Robot alone, and its winget
  manifest's `ReleaseDate` was the job's own day. It passed Chocolatey's automatic
  checks, and at 11:31 UTC on 21 October 2026 a moderator returned it to the
  maintainer ("Waiting for Maintainer to take corrective action"), asking that its
  `<authors>` also credit the original hMailServer's author, Martin Knafve, and that
  the same version be resubmitted.
- **Sent again on 22 October 2026, without the credit.** `chocolatey-publish` (job
  17362512835, in `master`'s pipeline 2973751489) sent 6.3.7 again at 01:06 UTC on 22
  October 2026 (HTTP 201), in the first package's place, with the fork's title,
  summary and description, but with authors and copyright that still name Progressive
  Robot alone: it says that it is the fork but does not credit Martin Knafve, the one
  fix the moderator asked for. It passed Chocolatey's automated validation at 01:39
  UTC; its automated verification failed at 01:44 UTC because the verifier's download
  of it from Chocolatey's own feed timed out (a gateway timeout, not a fault in the
  package), and its page again showed "Waiting for Maintainer to take corrective
  action". The feed still listed it as `Submitted`, not approved (read at 02:29 UTC
  that day).
- **Sent a third time on 23 October 2026, with the credit.** `chocolatey-publish` (job
  17399248551, in `master`'s pipeline 2978723670) sent 6.3.7 again at 00:59 UTC on 23
  October 2026 (HTTP 201), in the 22 October package's place, with Martin Knafve
  credited in its authors and copyright, while the feed still listed it as `Submitted`
  (point 7 above). It passed Chocolatey's automated validation at 01:32 UTC and its
  verification at 01:42 UTC.
- **Approved on 23 October 2026.** A moderator approved it at 11:56 UTC that day; the
  feed lists it as `Approved`, its virus scan `NotFlagged`, and `choco install
  hmailserver-progressiverobot` installs it. On a clean Windows Server 2025 (the
  installer smoke VM at its first snapshot) that afternoon, Chocolatey 2.7.4's `choco
  install hmailserver-progressiverobot` downloaded the release's installer, whose
  SHA-256 matched and whose Authenticode signature was Valid (Progressive Robot Ltd),
  installed 6.3.7 with the service running and ports 25, 110 and 143 answering, and
  `choco uninstall hmailserver-progressiverobot` removed it, leaving no uninstall
  entry and no service. Every new version waits for a moderator the same way.

### Snap Store

`snap-upload` (`build/ci/channels/snap-upload.py`) takes the snaps that
`snap-build-amd64` and `snap-build-arm64` made in the same pipeline
([Snap.md](Snap.md), `.gitlab/ci/snap.yml`) and uploads them to the `edge` and
`candidate` channels; `stable` is promoted by hand, once a maintainer has installed
the candidate (`snapcraft release hmailserver <revision> stable`). It runs on linix
in the Ubuntu 24.04 image the builds use, with snapcraft as they have it - from the
Snap Store at a pinned revision, without snapd (`build/ci/snap-tools.sh`), which
`snapcraft upload` does not need.

The contract with the two builds:

- each leaves `snap/hmailserver_<version>_<arch>.snap` (`amd64`, `arm64`) in its
  artifacts, `<version>` the release's;
- and, when it built from the release's source tarball,
  `snap/hmailserver_<version>_<arch>.snap.source`: one line, `<SHA-256>
  hmailserver-<version>.tar.gz`.

`snap-upload` holds each snap to the release - its name's version, and the name and
version in its `meta/snap.yaml` - and to what it was built from: the `.source`
line's SHA-256 must be that of the release's tarball, which it verifies; without a
`.source` line, the pipeline's own commit must be the release tag's, checked as the
release job checks a tag. A snap of `master` after the release is refused.

The snap itself is not a signed file of the release: it is the builds' output in
this pipeline, built from the verified tarball (or the tag's tree) with `master`'s
`snap/` packaging, and what ties it to the release is that check, not a Sigstore
bundle of its own.

Then the store's public channel map (`api.snapcraft.io`, no credential): a channel
is a pointer to a revision, and every device following it moves to whatever
revision it points at, older or not, so a release older than what `edge` or
`candidate` holds for that architecture is refused, and where both hold this
release already that snap is not uploaded again (a rebuilt snap is other bytes, a
new revision of the same version; a change to the snap's packaging for a release
the store has is uploaded by hand). Then `snapcraft upload
--release=edge,candidate`, which reads `SNAPCRAFT_STORE_CREDENTIALS` from its
environment itself.

- **The owner, once.** An Ubuntu One account with two-factor authentication; the name
  `hmailserver` registered (`snapcraft register hmailserver`, with the comment the
  maintainers drafted); and, in a terminal on Linux (snapcraft as `build/ci/snap-tools.sh`
  installs it, [Snap.md](Snap.md), The Snap Store), `snapcraft export-login
  --snaps=hmailserver --channels=edge,candidate --acls
  package_access,package_push,package_update,package_release --expires YYYY-MM-DD -`,
  whose output is `SNAPCRAFT_STORE_CREDENTIALS`. `--channels=edge,candidate` holds the
  credential below `stable`, which stays a person's step with their own login;
  `snap-upload` asks the credential what it may do (`snapcraft whoami`) and refuses
  one that could release to `stable`, or of which it is told nothing, before it
  uploads anything. The credentials carry the expiry date `--expires` gives; once past
  it the job fails with the store's refusal, and the same command makes new ones.

What the owner and the maintainers set
--------------------------------------

Every variable is a project variable (Settings, CI/CD, Variables), **Protected**, and
**Masked and hidden** where it is a secret; scope it to the job's environment where
the table says so. A masked value must be one line of letters, digits and `+ / = @ :
. ~ - _`, which is why the keys are base64.

| Variable | Secret | Environment | Value |
|---|---|---|---|
| `HM_CHANNEL_VERSION` | no | all | optional: a release to publish instead of the feed's latest (`6.3.6`) |
| `DOCKERHUB_USERNAME` | no | `container-registries` | `progressiverobot` |
| `DOCKERHUB_TOKEN` | yes | `container-registries` | a Docker Hub access token, Read & Write, for the repository |
| `DOCKERHUB_REPOSITORY` | no | `container-registries` | optional, default `docker.io/progressiverobot/hmailserver` |
| `QUAY_ROBOT_USER` | no | `container-registries` | the robot account, `hmailserver+<name>` |
| `QUAY_ROBOT_TOKEN` | yes | `container-registries` | its token |
| `QUAY_REPOSITORY` | no | `container-registries` | optional, default `quay.io/hmailserver/hmailserver` |
| `AWS_ACCESS_KEY_ID` | no | `container-registries` | only for ECR Public |
| `AWS_SECRET_ACCESS_KEY` | yes | `container-registries` | only for ECR Public |
| `ECR_PUBLIC_REPOSITORY` | no | `container-registries` | `public.ecr.aws/<alias>/hmailserver`, with the two above |
| `AUR_SSH_KEY` | yes | `aur` | `base64 -w0` of the private key |
| `LAUNCHPAD_SIGNING_KEY` | yes | `launchpad` | `base64 -w0` of the upload key's secret key |
| `LAUNCHPAD_SIGNING_PASSPHRASE` | yes | `launchpad` | optional |
| `LAUNCHPAD_PPA` | no | `launchpad` | optional, default `hmailserver/hmailserver` |
| `LAUNCHPAD_SERIES` | no | `launchpad` | optional, default `noble resolute` |
| `COPR_WEBHOOK_URL` | yes | `copr` | the package's custom webhook |
| `OBS_RUNSERVICE_TOKEN` | yes | `obs` | the runservice token |
| `OBS_PROJECT` | no | `obs` | the OBS project that holds the package (`home:<account>`) |
| `CHOCO_API_KEY` | yes | `chocolatey` | the community.chocolatey.org API key |
| `HM_CHOCOLATEY_ID` | no | `chocolatey` | `hmailserver-progressiverobot` (set on 20 October 2026, and the default); `hmailserver`, the original's id, is refused |
| `SNAPCRAFT_STORE_CREDENTIALS` | yes | `snap-store` | the output of `snapcraft export-login` |

With `glab`, signed in as a maintainer, a secret goes in from standard input and is
never typed on a command line:

```sh
base64 -w0 aur | glab variable set AUR_SSH_KEY --protected --masked --scope aur
```

Publishing a release to the channels
------------------------------------

On the `master` pipeline after the release is published (the update feed lists it):

1. Start `channels-verify`. It downloads the release's source tarball and
   installer, verifies both, and publishes nothing: its log and
   `verification.json` show what every channel will take.
2. Start each channel's job whose account exists. Each says what it published and
   where; a job whose channel is not set up says so and passes.
3. After the release, and for any later one, the same buttons publish the newest
   release. To publish an older one to a channel that has none newer, set
   `HM_CHANNEL_VERSION` first, and remove it afterwards: Copr and OBS take the
   latest release alone and refuse it, and the AUR and the snap refuse a release
   older than the one they have.

When a job fails, its log says why before anything is sent - the release not in the
feed, a file that does not verify, a credential refused. The container registries
are the exception, since each destination is published in turn: a job that fails at
Quay after Docker Hub is simply run again, and repeats nothing harmful.

Trying a channel on a laptop
----------------------------

Each script has a `--self-test` that needs no network, no credential and no tool
(`chocolatey-publish.ps1 -SelfTest` for the PowerShell one); the `channels-check`
job runs them all, in merge request and branch pipelines, when a channel's files
change, and proves the verification with the pinned cosign itself. `.gitlab-ci.yml`
counts as one of those files, since it pins that cosign, and so does
`.gitlab/ci/no-github.yml`, which the job takes first.

`build/ci/channels/fake-sigstore.py` is a Sigstore of the jobs' own, as
`hmailserver/test/RegressionTests/Shared/FakeSigstore.cs` is the server's: a
Fulcio-shaped authority with an embedded SCT, a Rekor-shaped log with an inclusion
proof, and a TrustedRoot that `cosign verify-blob --trusted-root` takes. Its
`fixture` command signs files as a release, serves the feed, the release and the
registry on the loopback, and writes the `fixture.json` that `HM_CHANNELS_FIXTURE`
names; with it, a job runs end to end on a laptop:

```sh
python3 build/ci/channels/fake-sigstore.py fixture --dir /tmp/fx --version 6.3.6 \
    --set aur_remote=/tmp/aur.git hmailserver-6.3.6.tar.gz &
git init --bare /tmp/aur.git
HM_CHANNELS_FIXTURE=/tmp/fx/fixture.json AUR_SSH_KEY=$(base64 -w0 some-key) \
    python3 build/ci/channels/aur-publish.py --out /tmp/aur-out
```

`--tamper <file>` changes a file after it is signed and `--other-signer <file>`
signs it as a branch's pipeline: a job must refuse both. `HM_CHANNELS_FIXTURE`,
`HM_CHANNELS_COSIGN` and `HM_CHANNELS_REGCTL` are refused in any CI job, so no
variable can point a channel at another feed, another trust or another tool.

What was tried
--------------

On 28 September 2026, on a laptop (Windows 11, WSL Ubuntu, Docker in WSL):

- **cosign 2.6.5 itself**, taken out of `HM_COSIGN_IMAGE` by the script (SHA-256
  `8d5b7d00...5d48`, the same on Windows and in an Arch container), verified
  fake-sigstore's bundle of a file
  (`Verified OK`), and refused the file changed after signing, another tag's
  pipeline, a branch's pipeline (a rehearsal), another issuer, another Sigstore,
  and a log entry the log did not sign; the script refused a certificate at the
  group's path without the project's id, and one with another project's id.
- **Every job without its variable** said `not configured` and exited 0.
- **Every job with a fixture credential, against a fixture release whose tarball
  was changed after signing and whose installer was signed by a branch**, refused
  with cosign's own reason before it logged in, built or sent anything:
  `channels-verify`, `chocolatey-release`, `aur-publish`, `ppa-upload`, `copr-build`
  (both halves), `obs-publish` and `snap-upload`.
- **`registry-mirror`** refused `docker.io/library/busybox:1.37.0`, which nobody
  signed ("no signatures found"), before any login. Against
  `registry.k8s.io/pause:3.10.1`, signed keylessly by Kubernetes' release account
  in the same tag form, with that account as the identity: cosign verified the
  signature by digest with the public Sigstore trust, and the job refused because
  no CycloneDX SBOM is attested, copying nothing. The certificate read out of its
  `.sig` manifest names that account and no repository id, and `regctl image copy
  --digest-tags` of it by digest into an OCI layout carried
  `sha256-<digest>.sig` with it, at the same digest. That was the job before 29
  September, which copied the signature so; since then it copies the manifests it
  verified, out of its own layout ("What it copies", above).
- **The AUR, end to end**, in a clean `archlinux:base-devel` container: against a
  signed fixture release of this tree's tarball, `aur-publish` verified it, rendered
  the PKGBUILD, made `.SRCINFO` with `makepkg --printsrcinfo` as an unprivileged
  user, and pushed both to a local bare repository standing for the AUR; run again,
  it found the AUR had them byte for byte and pushed nothing; after the PKGBUILD
  changed on the branch it pushed `6.3.5-2`. `namcap` found nothing in the
  PKGBUILD. The first `makepkg -si` failed: Arch's default `-flto=auto` made the
  final link of the server a full LTO link, which passed 6 GB and was killed, so the
  PKGBUILD now says `options=('!lto')`. From the repository, `makepkg -si` then
  built the server with clang 22 and Arch's Boost 1.92 (two compile jobs) and
  installed `hmailserver 6.3.5-2`: no `hmailserver-update`, the user
  and directories made by pacman's sysusers and tmpfiles hooks. The installed server,
  started as the unit starts it (as `hmailserver`, `--foreground`) against
  PostgreSQL in the same container with every port moved up by 20000 (WSL shares
  its ports with Windows, where the laptop's own server listens), answered
  `220 ... ESMTP` and `* OK IMAPrev1`.
- **The PPA and Copr past verification**, in WSL (Ubuntu 26.04: devscripts 2.26.7,
  dput 1.2.4, rpm 6.0.1), against a signed fixture release whose tarball carries the
  Debian packaging and RPM spec of SourcePackages.md as that item stood that day:
  `ppa-upload` repacked the tarball with `mk-origtargz`, built and signed
  `6.3.5+ds-1~noble1` and `6.3.5+ds-1~resolute1`, sent the orig with the first alone,
  and `dput`'s local method took both (good signatures on each `.changes` and `.dsc`;
  `dpkg-source -x` unpacks them). `copr-build.py srpm` made
  `hmailserver-6.3.5-1.src.rpm` from the verified tarball, its spec and `Source1`;
  against the changed tarball it refused and made nothing.
- **Chocolatey**, on Windows with Chocolatey's CLI: the job's script rendered the
  package from 6.3.4's signed installer (Authenticode Valid, "Progressive Robot
  Ltd"), `check-package-manifests.py --offline` passed it, and `choco pack` made
  `hmailserver.6.3.4.nupkg` and, with `HM_CHOCOLATEY_ID`,
  `hmailserver-progressiverobot.6.3.4.nupkg`. Nothing was pushed.
- **`channels-check` in its own image** (Microsoft's .NET SDK 10.0 by digest, with
  `CI` set): every self-test, the PowerShell one under pwsh on Linux, cosign fetched
  from its pinned image behind `.no-github`, and the fixture Sigstore proved with
  it: 153 cases, none failed.
- **GitLab's CI lint** with a dry run of the merged configuration: valid for `master`
  (the ten jobs of this file), `batch42` (`channels-check` alone) and `v6.3.4` (none).
- **On 29 September, the re-check**: the certificate reader took the repository id
  `1267388815` out of a real Fulcio certificate (6.3.3's installer bundle), and
  refused this project's id planted in a public key in front of another; the
  snap's channel-map reader read the store's live answer for another snap, and
  `hmailserver`'s (none yet); `chocolatey-publish.ps1 -NoPush` packed 6.3.4's
  installer as `hmailserver-progressiverobot` again, with choco's own lines in the
  log. Then `registry-mirror`'s reading, against a local `registry:2` with an image
  cosign 2.6.5 signed and attested with a key: the layout the job writes, fetched
  with regctl 0.11.6, verified with `cosign verify --local-image` and
  `verify-attestation --local-image`; its `.sig` and `.att` copied out of it to
  another repository, where they were the same digests and verified again; and a
  registry that serves the signature tag as an index under `"MANIFESTS"` with
  `"layers"` beside it had cosign verify the index's child while the reading before
  this one took the layer beside it (`registry:2` refuses to store such a manifest;
  a registry of its own served it). And keyless, with `fake-sigstore.py`'s authority
  handed to cosign by the variables its tag form reads, offline: the job took an image
  this project's pipeline signed and attested, and refused one another project's
  pipeline signed, or attested, at the identity - each of which cosign itself verified
  (`registry-mirror.py --self-test --cosign`, which `channels-check` runs).
- **On 21 October 2026, Docker Hub.** `registry-mirror`, configured for Docker Hub
  that day and run on `master`, refused 6.3.7 before it copied anything:
  `registry.gitlab.com/hmailserver/hmailserver:sha256-2a5de4eb....att could not be
  read: the image has no attestation there`. 6.3.7's image is the index
  `sha256:2a5de4ebd04cd1e39f69ebeca3e905dd80217c3178132bd17a07880845ae629f`, and the
  job, written for an image of one platform, looked for an SBOM attestation beside
  the index, where the release's pipeline attests none; it now verifies an index as
  "What it verifies" says. The same day 6.3.7 was copied to
  `docker.io/progressiverobot/hmailserver` by hand with regctl, by digest: the index
  with both platforms' images at the tags `6.3.7`, `6.3` and `latest`, and the
  index's signature and each platform's signature and SBOM attestation at their
  `sha256-<digest>.sig` and `.att` tags, every digest there equal to the source's -
  the index's `.sig`
  `sha256:9a917f2d8c885052dc030bba6d8f2c515bf7539d6e2e2e127c923a8bac405e36`;
  linux/amd64's (`sha256:2f5de02f...`) `.sig`
  `sha256:76af2d5d146d9a806ec0163991b183352e25310c3e0bc13feda8168b5557aa5f` and `.att`
  `sha256:fd3764cb617b79dd598c0875a19ad5bf3f4550d5dc06dd695f5178e723653475`;
  linux/arm64's (`sha256:a33f18b5...`) `.sig`
  `sha256:87fe655200b13047ca74bf15d841be5a19f90d1aea18847892d431a95d87d777` and `.att`
  `sha256:11eaf2c57f383452e197c024d2783e0231263ba9690b30978b007e1c6f837080`. cosign
  2.6.5 (the one the channel jobs pin) verified there, without signing in, the
  index's and both platforms' signatures and both SBOM attestations as
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.7`; and
  the job's own verification, after the change, run without signing in against both
  registries with the pinned cosign and regctl, took the index and both platforms
  there as that identity, with the project's id, `86730559`, in every signing and
  attesting certificate. The repository is public; its description and overview say
  that it is Progressive Robot's fork of hMailServer. The job's own copy to Docker
  Hub has not run since.
- **On 21 October 2026, OBS, Copr and the PPA**, each from its job in `master`'s
  pipeline 2966261322, for 6.3.7. `obs-publish` (job 17306495327) verified the
  release's tarball and started the package's service: `download_url` followed
  GitLab's permanent link to 6.3.7's tarball, `extract_file` took the spec and its
  `Source1`, `hmailserver.sysusers`, out of it, and `download_files` fetched the
  spec's `Source0`, `hmailserver-6.3.7.tar.gz`, the same bytes (the package's
  expanded sources, as OBS's API gives them without signing in); OBS built it for
  Leap 16.0, x86_64 and aarch64, and Tumbleweed, x86_64, and published both
  repositories, signed with the key OBS made for the project,
  `BA0868309CFBD36498CE41D22F0B9419E67EE770`. `copr-build` (job 17306495326)
  verified the tarball and called the webhook; in Copr's builder `.copr/Makefile`
  ran `dnf -y install python3 rpm-build`, and `copr-build.py srpm` took cosign out of
  its pinned image on gcr.io, verified the tarball again as the `v6.3.7` tag's
  pipeline (repository id `86730559`) and made `hmailserver-6.3.7-1.src.rpm`, which
  Copr's build 11180395 built in all six chroots. `ppa-upload` (job 17306495325) sent
  `6.3.7+ds-1~noble1` and `6.3.7+ds-1~resolute1`, signed with the upload key;
  Launchpad accepted both at 01:42 UTC, built each for amd64 and arm64, and publishes
  them, signed with the PPA's own key, `F1701CED8D88F87E19B08F7B0ADD4C9C6D462C8C`.

Not tried: a push to Quay.io, ECR Public and the AUR, whose accounts do not exist;
and `registry-mirror`'s own copy of a release to Docker Hub (6.3.7 there was copied
by hand, above). `chocolatey-publish` has run on bench150, and pushed 6.3.7
([Chocolatey](#chocolatey)). `snap-upload` ran for 6.3.7 on 18 October 2026 (job
17257943981) and stopped before it reached the store or the credential, the image
having no `git` for its check of the tag; the store's answer to an upload is still
untried ([Snap.md](Snap.md), "The Snap Store").
