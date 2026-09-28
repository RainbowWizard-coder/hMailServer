# The container image and the Helm chart

hMailServer runs on Linux in a container from the project's own image, and on
Kubernetes from the project's own Helm chart. Both are made by the release's GitLab
CI pipeline from the release's own files, and signed with Sigstore by that pipeline,
like every other file of the release:

| | Where |
|---|---|
| The image | `registry.gitlab.com/hmailserver/hmailserver:<version>`, and for a release without a suffix `:<major>.<minor>` and `:latest` too |
| The chart | `https://updates.progressiverobot.com/helm/`, the chart `hmailserver`, at the release's version; and the release's file `hmailserver-chart-<version>.tgz` |
| The Compose file | `hmailserver/source/Server/platform/packaging/docker/docker-compose.yml` |
| How to run them | `hmailserver/source/Server/platform/packaging/README.md`, "Install and get running"; every variable the image reads is in [EnvironmentConfiguration.md](EnvironmentConfiguration.md) |

The image's address is the project's path in the group `hmailserver`, which is where
the image stays. The maintainers decided on 27 September 2026 to push the first
image only once the project is at that path, because a container repository's
address is always its project's: an image pushed from the project's first path,
`registry.gitlab.com/progressiverobot/hmailserver`, would have changed its address
under everyone who pulled it. So no image is pushed from that path, and a release
built by hand, 6.3.4 among them, has none. The images published before GitHub
suspended the project's account on 18 September 2026 were on `ghcr.io`, which has
answered 404 since.

## The image

The Dockerfile is `hmailserver/source/Server/platform/packaging/docker/Dockerfile`.
The image is the release's `.deb` for x86-64 installed on Ubuntu 24.04, pinned by
digest, running as the package's own user, `hmailserver`, with the one capability
the mail ports below 1024 need set on the binary rather than given to the container.
It has linux/amd64 only; an arm64 image is still to come (Roadmap3.md, section 1b).

| | |
|---|---|
| Ports | 25, 110, 143, 465, 587, 993, 995, 4190 and 8045 (the REST API, the Control Deck and the webmail) |
| Volumes | `/var/lib/hmailserver` (the message store), `/var/log/hmailserver`, `/etc/hmailserver` (`hMailServer.ini`) |
| Health | port 25 accepts a connection |
| Start | `docker-entrypoint.sh`: the environment becomes the configuration, the database is created or upgraded, and the server runs in the foreground |
| Kind of installation | `HM_INSTALL_KIND=container`, for the update check (Updates, below) |

The database is not in the image: point `HM_DB_*` at a PostgreSQL or MariaDB
server. `HM_DB_WAIT_SECONDS` holds the entrypoint until that server accepts a
connection, for a database started beside the container with nothing else to wait
for it. The REST API is the administration on Linux, and the server requires TLS
on it anywhere but loopback, so a reachable API means a certificate mounted in
(`HM_REST_CERTIFICATE_FILE`, `HM_REST_PRIVATE_KEY_FILE`), or the self-signed pair
`hmailserver --apply-new-install-defaults` makes for a new installation - which a
container is not given by itself, since only the packaged systemd unit's first
start applies a new installation's defaults ([SecureDefaults.md](SecureDefaults.md)).

The image's labels, which a registry and Artifact Hub show: the OCI ones
(`org.opencontainers.image.title`, `description`, `licenses`, `vendor`, `url`,
`source`, `documentation`, and from the build `version`, `revision` and `created`,
the commit's time) and Artifact Hub's (`io.artifacthub.package.readme-url`, this
document; `license`, `keywords`, and `prerelease` for a version with a suffix).
`build/ci/linux-image.sh` fails a build without the three Artifact Hub requires.

### Checking an image

The image is signed by its digest, and its software bill of materials (CycloneDX)
is attested to it, both as the release's pipeline. With cosign 2.4 or later:

```
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver:<version>

cosign verify-attestation --type cyclonedx \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver:<version> | jq -r .payload | base64 -d | jq .predicate
```

The identity is the project's path when the image was signed: an image is pushed and
signed only from the group's path, so it is the group's. The `.deb` the image was
built from may have been signed before the project moved, as
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`;
the release page's verification section says which.

## The Helm chart

The chart is `hmailserver/source/Server/platform/packaging/helm/hmailserver`. Its
README.md, which Artifact Hub shows, says what it makes and what it needs. From the
project's repository:

```
helm repo add hmailserver https://updates.progressiverobot.com/helm/
helm install mail hmailserver/hmailserver \
  --set database.host=... --set database.password=... \
  --set administrator.password=... --set rest.tlsSecretName=mail-tls
```

Each chart there is a release's: its version and its appVersion are the release's,
and it installs that release's image unless `image.tag` names another. A
pre-release's chart has a SemVer pre-release version, which Helm lists only to
`--devel`. In the tree the chart's version is a placeholder, `0.1.0`, and its
appVersion `latest`: `build/check-linux-version-stamp.sh` refuses a release's number
written there.

### Checking a chart

The chart is a file of its release, `hmailserver-chart-<version>.tgz`, with its
Sigstore bundle beside it, `hmailserver-chart-<version>.tgz.cosign.bundle`:

```
cosign verify-blob \
  --bundle hmailserver-chart-<version>.tgz.cosign.bundle \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  hmailserver-chart-<version>.tgz
helm install mail ./hmailserver-chart-<version>.tgz --set database.host=...
```

The identity is the project's path when the release was signed, as for every
file of the release: a release tagged before the project moved into the group
was signed as
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
and the release page's verification section says which.

`helm pull hmailserver/hmailserver --version <version>` saves the same file: the
repository's index names each chart by its URL in the project's package registry,
the file the release page links, with its SHA-256. The chart carries no Helm
provenance file (`.prov`, a PGP signature): its signature is Sigstore's, as every
other file's is, so no key of the owner's is needed.

## Artifact Hub

Artifact Hub (artifacthub.io) indexes charts and images and hosts neither. The
owner's account, `progressiverobot89`, and the organization `hmailserver` exist.

- **The chart.** Once a release's chart is on the Pages site, the maintainers add a
  repository of kind Helm charts to the organization with the URL
  `https://updates.progressiverobot.com/helm/`. Artifact Hub gives it a repository
  ID, which goes into `ARTIFACTHUB_REPOSITORY_ID` in `build/ci/helm-repo.py`: the
  pages job then writes it into `helm/artifacthub-repo.yml` beside the index, which
  gives the listing its Verified publisher label. The chart's annotations name its
  licence (`artifacthub.io/license`), its image (`artifacthub.io/images`, stamped
  with the release's version), its links, and whether it is a pre-release. The
  Official label is requested through a GitHub issue, and is not.
- **The image.** A repository of kind Container images, with the image's address,
  lists it from its labels. Its Verified publisher label needs the same metadata
  file pushed to the image's repository under the tag `artifacthub.io`; the tool
  for that is `regctl`, from its image on Docker Hub (`regclient/regctl`), since
  ORAS comes from GitHub. That push is not written yet.

## Updates

The server does not update itself in a container: a new release is a new image. From
6.3.6 the image sets `HM_INSTALL_KIND=container`, and the server's update check,
finding a newer release, says so and names the image to pull, and downloads and
installs nothing ([UpdateFeed.md](UpdateFeed.md)). Then:

- Compose: set the new version on the `image:` line, `docker compose pull`,
  `docker compose up -d`; the entrypoint upgrades the database at the start.
- Helm: `helm repo update`, then `helm upgrade mail hmailserver/hmailserver
  --reuse-values`.

## How the pipeline makes them

### The image jobs

`.gitlab/ci/container-image.yml`, in every protected `v*` tag's pipeline, after
`release-publish`:

| Job | Where | What it does |
|---|---|---|
| `container-image-deb` | GitLab's shared runners, in cosign's image (`HM_COSIGN_IMAGE`); **started by hand** | Takes `hmailserver_<version>_amd64.deb` and its SBOM from the published release, with their Sigstore bundles, and verifies each with `cosign verify-blob` as signed by this project's pipeline for the tag, at either of the project's two paths, with the project's id `86730559` required in the certificate at the group's path (`build/ci/container-image-deb.sh`) |
| `container-image` | linix, in kaniko's image | Builds the Dockerfile from that `.deb`, checks the image's user, entrypoint, healthcheck, ports, file capability, `HM_INSTALL_KIND` and labels, and pushes `:<version>` (and `:<major>.<minor>` and `:latest` for a release without a suffix) (`build/ci/linux-image.sh`, `HM_IMAGE_PUSH=1`) |
| `container-image-sbom` | linix, in the Linux CI image | Syft's reading of the pushed image by its digest, with what the `.deb`'s own SBOM names inside the binary (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | GitLab's shared runners, in cosign's image | Signs the image by its digest, attests its SBOM, and verifies both (`build/ci/container-image-sign.sh`) |
| `container_scanning` | GitLab's shared runners | GitLab's Container Scanning over the pushed image by its digest; a finding is a warning, and its report is the job's artifact |
| `container-image-start` | linix, in the Linux CI image | Starts the pushed image by its digest as a service beside PostgreSQL, and asks it for SMTP on 25 and for the REST API over TLS (The start test, below) |

**Why the .deb comes from the release.** The image jobs may run weeks after the
tag - after the project's move - and by then `release-sign`'s artifact, which held
the signed files, may have expired. The published release does not expire, and its
files are the ones users download, so the image is built from those very bytes,
held to their signatures. `container-image-deb`'s `wget` is busybox's, which does
not check a server's TLS certificate: nothing it sends is a credential (the project
is public, and its releases and package registry answer anonymously), and a file
changed on the way fails its signature and stops the job.

### Publishing

The switch is the project's CI/CD variable `HM_PUBLISH_IMAGE`, **read when the jobs
run**, not when the pipeline is made: `container-image-deb` and `linux-image.sh`
refuse, saying so, with exit status 3, while it is anything but `1`. So the jobs
are in every `v*` tag's pipeline whatever the variable was at the tag, and a release
published before the project's move has its image pushed from its own pipeline
after it:

1. The project is at `gitlab.com/hmailserver/hmailserver` (Roadmap3.md, section 8,
   the move into the group), and the registry's cleanup policy keeps the five
   documented tag patterns (ContinuousIntegration.md, "What the owner sets on the
   registry").
2. A maintainer adds `HM_PUBLISH_IMAGE` = `1` as a project CI/CD variable
   (Settings, CI/CD, Variables; Protected, since a `v*` tag is a protected ref), or
   through the API.
3. In the release's tag pipeline, start `container-image-deb`; the rest follow.

It is a project variable because the project refuses variables given to a pipeline
or to a schedule (`ci_pipeline_variables_minimum_override_role` is `no_one_allowed`),
so nobody can turn the jobs on for one run.
Nothing committed sets it, and no rule reads it: `build/check-release-chain.py`
fails a CI file that does either, and holds the two scripts to their refusal.
`container-image` pushes only at the group's path: in a pipeline whose project is
still at its first path it refuses, exit 3, and pushes nothing, so a switch set
before the move cannot put an image at the address the move would change.
`container-image-sbom` and `container-image-start` run in linix's CI image of the
tag's commit (`hmailserver-ci/linux:<release>-<hash>`), which is built there and
never pulled: keep that tag on linix until the release's image jobs have run.
`container-image-deb` is not allowed to fail, because GitLab starts a job whose
need was allowed to fail after that need has failed: the image would be built after
a refusal, or after a bundle that did not verify. So until someone starts it, the
tag's pipeline shows as blocked on it - the release's image is still to be pushed -
and a refusal fails it; running it again once the variable is set lets the jobs
after it go on.

### When a job fails

`container-image` pushes before it checks, because kaniko writes the tarball it
checks in the same run as the push; `container-image-start` runs after the push.
If either fails, the tags it pushed name an image that did not pass: the job's log
names them, and they are deleted by hand (Deploy, Container registry), with the
signature and attestation tags (`sha256-<digest>.sig` and `.att`) if
`container-image-sign` ran. Then the fix is released as a new version; a tag is
spent once.

### The start test

`build/ci/container-image-start-test.sh` starts the image as a user starts it,
through its own entrypoint, with the database, the administrator password, the
REST listener and `HM_DB_WAIT_SECONDS` from the environment, and the command
`hmailserver --apply-new-install-defaults && exec hmailserver --foreground`: the
new installation is secured - a self-signed certificate, STARTTLS - before the
server starts, and the REST listener is pointed at that certificate, so the test
needs no certificate from outside. It then asks:

- port 25: the banner, `EHLO` answered with `STARTTLS` among the extensions;
- the REST API on 8045 over TLS: `GET /api/v1/status` refused with 401 without
  credentials, answered with the release's version as Administrator, and not
  answered over plain HTTP.

In the job, the runner starts PostgreSQL and the image as the job's services on a
network of the job's own (`FF_NETWORK_PER_BUILD`), and `CI_DEBUG_SERVICES` puts
both services' output in the job's log.

**By hand**, on any machine with a Docker daemon - linix, or a laptop's WSL - the
script starts the same two containers itself and removes them afterwards:

```
# an image from a registry, or one built from the Dockerfile with a .deb in packages/
HM_START_VERSION=<version> bash build/ci/container-image-start-test.sh --docker <image>

# the image linux-image built without pushing, from its tarball (HM_KEEP_IMAGE=1)
docker load -i linux-image/hmailserver-image.tar
HM_START_VERSION=<version> bash build/ci/container-image-start-test.sh --docker hmailserver:<version>
```

`--keep` leaves both containers running and says how to remove them. On 28
September 2026 it was run this way in WSL (Ubuntu 26.04, Ubuntu's docker.io 29.1.3)
against the Dockerfile of this tree built from 6.3.4's published `.deb`: the
entrypoint waited 8 seconds for PostgreSQL, created the schema, applied the four
REST settings and the new installation's defaults, and the server answered SMTP
with STARTTLS and the REST API over TLS as `6.3.4-B43`; asked for another version,
the test failed and printed the image's log. Its job mode was run the same day with
three containers on one Docker network standing in for the runner - PostgreSQL as
`db`, the image as `mail` with the service's command, and the script in a third -
each given all of the job's variables, as GitLab gives a job's variables to its
services: it passed, and asked for another version it failed naming the pushed
tags to delete.

### Packaging and serving the chart

`.gitlab/ci/helm-chart.yml`'s `helm-chart-package` job (GitLab's shared runners,
Alpine's image and its `helm`) runs for a protected `v*` tag and a protected
`batch*` branch. `build/ci/helm-chart-package.sh` takes the version from the tag -
which must be Version.h's, with or without a SemVer suffix (`v<version>-rc1`, not
`v<version>rc1`) - or on a branch from Version.h; stamps a copy of the chart's
`artifacthub.io/images` and `artifacthub.io/prerelease`; lints it; packages it with
`helm package --version <v> --app-version <v>`; and reads the package back
(`helm show chart`, `helm template`), holding it to what was stamped. It leaves
`helm-chart/hmailserver-chart-<version>.tgz` and its SHA-256, which the release job
collects as a file of the release; `release-sign` signs it and `release-publish`
uploads it with the others.

The Pages site's repository is written after the feed, from the feed's
`releases.json`, by `build/ci/helm-repo.py --site public`: `helm/index.yaml`, with
one entry per release that carries a chart, each its own Chart.yaml as the package
holds it, the file's URL in the package registry, its SHA-256 and the release's
date; `helm/artifacthub-repo.yml`; and `helm/index.html` for a person. Each chart is
downloaded once, held to the size and SHA-256 `releases.json` gives, and read, so
that the index describes what is there; a chart that is not this project's, or not
at its release's version, stops the job and the site keeps what it had.
`helm-repo.py --self-test` proves the selection, the reading and the refusals with
no network; with `--example DIR` it writes a repository to try Helm against. On 28
September 2026, in Alpine 3.24 with its Helm 3.19.0: the chart linted and rendered as
`chart-and-compose` asks; `helm-chart-package.sh` packaged it for a branch, for
`v6.3.5-rc1` (a pre-release, marked so) and refused `v6.3.9`, `v6.3.5rc1` and a
`Chart.yaml` whose image line had changed; and the example repository, served by
`python3 -m http.server`, was added with `helm repo add`, searched, and the chart
pulled byte for byte and rendered with the release's image.

## What is not shown yet

- No image has been pushed, signed, scanned or started by the pipeline: the first
  is the first release whose image jobs run with `HM_PUBLISH_IMAGE` set. The start
  test's GitLab half - the runner starting the services on a network of the job's
  own, the image named by the dotenv report's `HM_IMAGE`, the job's variables
  reaching the services - is first run there; its checks, and a stand-in for that
  half, were run by hand, as above.
- No chart has been published or listed on Artifact Hub.
- The image is linux/amd64 only.
