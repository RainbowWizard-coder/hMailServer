Release channels
================

Where a release of hMailServer is published besides its own release page, and how.
Each channel has a job of its own in `.gitlab/ci/channels.yml`, started by hand on
`master`, which publishes a signed release there once the channel's account exists.
The Windows installer's other package managers, winget and Chocolatey's history, and
the project's own apt and dnf repositories are in
[PackageManagers.md](PackageManagers.md); the command-line clients' PyPI and
PowerShell Gallery packages in [CommandLinePackages.md](CommandLinePackages.md).

**None of these channels is live yet.** Each job is written and was run on a laptop
against a fixture release (below, [What was tried](#what-was-tried)); each says "not
configured" and passes until the owner has made the channel's account and a
maintainer has stored its credential. No release before 6.3.6 can be published to
the AUR, the PPA, Copr or OBS: they build from the release's source tarball
([SourceTarball.md](SourceTarball.md)), and the Debian and RPM packaging inside it
([SourcePackages.md](SourcePackages.md)), which 6.3.6 is the first to carry.

| Channel | What a user types | Job | Credential (project CI/CD variable) |
|---|---|---|---|
| Docker Hub | `docker pull progressiverobot/hmailserver:<version>` | `registry-mirror` | `DOCKERHUB_USERNAME`, `DOCKERHUB_TOKEN` |
| Quay.io | `docker pull quay.io/hmailserver/hmailserver:<version>` | `registry-mirror` | `QUAY_ROBOT_USER`, `QUAY_ROBOT_TOKEN` |
| Amazon ECR Public (optional) | `docker pull public.ecr.aws/<alias>/hmailserver:<version>` | `registry-mirror` | `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `ECR_PUBLIC_REPOSITORY` |
| Arch User Repository | `git clone https://aur.archlinux.org/hmailserver.git && cd hmailserver && makepkg -si` | `aur-publish` | `AUR_SSH_KEY` |
| Ubuntu PPA | `sudo add-apt-repository ppa:hmailserver/hmailserver && sudo apt install hmailserver` | `ppa-upload` | `LAUNCHPAD_SIGNING_KEY` |
| Fedora Copr | `sudo dnf copr enable <owner>/hmailserver && sudo dnf install hmailserver` (the owner's Fedora account, or a group of it) | `copr-build` | `COPR_WEBHOOK_URL` |
| openSUSE Build Service | `zypper addrepo` of the project's OBS repository, then `zypper install hmailserver` | `obs-publish` | `OBS_RUNSERVICE_TOKEN`, `OBS_PROJECT` |
| Chocolatey | `choco install hmailserver` | `chocolatey-release`, then `chocolatey-publish` | `CHOCO_API_KEY` |
| Snap Store | `sudo snap install hmailserver --candidate` | `snap-upload` | `SNAPCRAFT_STORE_CREDENTIALS` |

How a channel job publishes
---------------------------

Every job below works the same way (`build/ci/channels/release-files.py`, which each
loads):

1. **Only on `master`, only by hand.** The jobs are created in a protected `master`
   branch pipeline and nowhere else: never for a tag, a merge request, a schedule or
   the update feed's downstream pipeline. Each is manual and allowed to fail, so a
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
   project moved, with the issuer `https://gitlab.com`. At the group's path the
   certificate must also carry the project's id, 86730559 (Fulcio's source repository
   identifier), and any id but that one is refused at either path: the rule every
   server applies to a live update (`SigstoreVerifier.cpp`). A file that does not
   verify stops the job before it logs in anywhere, and the log says what cosign
   said. The id is read from the certificate's extensions, not from wherever its
   bytes occur: a public key is bytes its holder chooses. The image is verified in
   the registry the same way (below); the snap, which is no file of the release, is
   held to the release's verified tarball or its tag instead (below).
4. **Dormant until set up.** Without its credential a job says `not configured`,
   names what the owner and a maintainer do, and exits 0 - before it reads the feed.
   A credential that is half there (a user without its token) fails the job.
5. **The tools from their own images.** `cosign` (2.6.5, the release chain's) and
   `regctl` (0.11.6) are static binaries the script takes out of their images,
   pinned by digest: `HM_COSIGN_IMAGE` in `.gitlab-ci.yml` (gcr.io) and
   `HM_REGCTL_IMAGE` in `.gitlab/ci/channels.yml` (Docker Hub). Every manifest and
   layer is held to its digest. Nothing is downloaded from GitHub, and the jobs on
   GitLab's shared runners take `.no-github` first (`.gitlab/ci/no-github.yml`).
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
   nothing. Two are not held to it: a Chocolatey version still waiting in moderation
   is not in the repository's feed, so a second press sends it again, and
   Chocolatey replaces the waiting package with it; and a second `copr-build` starts
   another Copr build of the same release. A release older than the channel's is
   refused by the AUR, the snap (per architecture, against what `edge` and
   `candidate` hold) and OBS (the latest release alone); on the PPA, Chocolatey and
   Copr an older version beside a newer one moves nobody back, since apt, choco and
   dnf take the highest; and the registries never move `:<major>.<minor>` or
   `:latest` back.

Each job has a GitLab environment of its own (`container-registries`, `aur`,
`launchpad`, `copr`, `obs`, `chocolatey`, `snap-store`), so each credential can be
scoped to the one job that uses it (Settings, CI/CD, Variables: "Environments"), and
a resource group, so two presses never publish at once.

The channels
------------

### Container registries: Docker Hub, Quay.io, Amazon ECR Public

`registry-mirror` (`build/ci/channels/registry-mirror.py`) copies the release's
image from `registry.gitlab.com/hmailserver/hmailserver`, the only place the
release's pipeline pushes it ([ContainerImage.md](ContainerImage.md)).

- **What it verifies.** `:<version>` is resolved to its digest; then, by that
  digest, `cosign verify` of the image's signature and `cosign verify-attestation
  --type cyclonedx` of its SBOM, both as the tag's pipeline (the identities above),
  and the project's id in the signing certificate, read from cosign's signature
  manifest (`sha256-<digest>.sig`).
- **What it copies.** `regctl image copy --digest-tags --referrers` of the source by
  its digest, so cosign's `.sig` and `.att` beside the image, and beside each
  platform's manifest of a multi-platform image, go with it. It copies to
  `:<version>` always, and to `:<major>.<minor>` and `:latest` only where the
  source's own tag of that name is the same digest: an older release mirrored late
  never moves them back. Then each tag at the destination is held to the digest,
  and the image there is verified again with cosign, anonymously, as a user would.
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
- **The owner, once.** Docker Hub: sign in as `progressiverobot`, make the public
  repository `hmailserver` and a Read & Write access token. Quay.io: a Red Hat
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
  signature alone.
- **The owner, once.** On Launchpad: the team `~hmailserver` and its PPA
  `hmailserver`, with arm64 builds enabled. The maintainers make a dedicated upload
  key (`gpg --quick-generate-key "hMailServer PPA upload <...>" ed25519 sign 2y`); the
  owner registers its fingerprint on his Launchpad account (a member of the team),
  and forwards Launchpad's encrypted confirmation mail for the maintainers to
  decrypt. `gpg --export-secret-keys <fingerprint> | base64 -w0` is
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

- `chocolatey-release` (a shared runner): `release-files.py verify
  "hMailServer-{version}-x64.exe"`, the installer verified as above, and
  `verification.json` handed on.
- `chocolatey-publish` (bench150, `build/ci/channels/chocolatey-publish.ps1`): the
  installer downloaded from the release and held to that SHA-256 and to a valid
  Authenticode signature; a version the community repository has, listed or not, is
  not sent again; `build/make-package-manifests.ps1` renders the package from those
  bytes, `choco pack` packs it, and the `.nupkg` is pushed to
  `https://push.chocolatey.org/` with an HTTP PUT and the key in its
  `X-NuGet-ApiKey` header (not `choco push`, which takes the key on its command
  line). A new package, and every new version, then waits for Chocolatey's
  moderation before it is listed.
- **The id.** `hmailserver` is upstream's package, 5.6.8.2574, last updated on 15
  January 2022 and now unlisted (the page does not say since when), maintained by
  `ferventcoder`, `gep13` and `chtof` ([PackageManagers.md](PackageManagers.md)). The
  project publishes under it once they add its account as a maintainer; otherwise
  the owner chooses `hmailserver-progressiverobot`, set as `HM_CHOCOLATEY_ID`.
- **The owner, once.** An account on community.chocolatey.org, its API key as
  `CHOCO_API_KEY`, and the id. bench150 needs Chocolatey's CLI
  (`https://community.chocolatey.org/install.ps1`); the job stops, saying so, where
  it is not.

### Snap Store

`snap-upload` (`build/ci/channels/snap-upload.py`) takes the snap that `snap-build`
made in the same pipeline ([Snap.md](Snap.md), `.gitlab/ci/snap.yml`) and uploads
it to the `edge` and `candidate` channels; `stable` is promoted by hand, once a
maintainer has installed the candidate (`snapcraft release hmailserver <revision>
stable`). It runs on the runner `snap-build` uses, where snapd and snapcraft do.

The contract with `snap-build`:

- `snap-build` leaves `snap/hmailserver_<version>_<arch>.snap` (`amd64`, `arm64`) in
  its artifacts, `<version>` the release's;
- and, when it built from the release's source tarball,
  `snap/hmailserver_<version>_<arch>.snap.source`: one line, `<SHA-256>
  hmailserver-<version>.tar.gz`.

`snap-upload` holds each snap to the release - its name's version, and the name and
version in its `meta/snap.yaml` - and to what it was built from: the `.source`
line's SHA-256 must be that of the release's tarball, which it verifies; without a
`.source` line, the pipeline's own commit must be the release tag's, checked as the
release job checks a tag. A snap of `master` after the release is refused.

The snap itself is not a signed file of the release: it is `snap-build`'s output in
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
  maintainers drafted); and, in a terminal on Linux with snapd (snapcraft is itself a
  snap), `snapcraft export-login --snaps=hmailserver --acls
  package_access,package_push,package_update,package_release -`, whose output is
  `SNAPCRAFT_STORE_CREDENTIALS`. The credentials carry an expiry date (`snapcraft
  export-login --expires` sets it); once past it the job fails with the store's
  refusal, and the same command makes new ones.

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
| `HM_CHOCOLATEY_ID` | no | `chocolatey` | optional: `hmailserver` (default) or `hmailserver-progressiverobot` |
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
change, and proves the verification with the pinned cosign itself.

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
  `sha256-<digest>.sig` with it, at the same digest.
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
  log.

Not tried, because the accounts do not exist: a push to Docker Hub, Quay.io, ECR
Public, the AUR, Launchpad, Copr, OBS, Chocolatey or the Snap Store; Launchpad's and
Copr's builds of what was made; Copr's `make_srpm` builder itself (its `dnf install`
line, and gcr.io from inside it); OBS's services themselves (whether `download_url`
follows GitLab's permanent-link redirect, whether `extract_file` takes two patterns,
and whether `download_files` sees the spec `extract_file` took out); the image copy
with our own image's `.att`, which no release has yet; `chocolatey-publish` on
bench150, which was busy; and `snap-upload` with snapcraft, which runs where
`snap-build` does.
