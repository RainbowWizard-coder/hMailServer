# The container image and the Helm chart

This is Progressive Robot's fork of hMailServer, not the original. The original
hMailServer is Martin Knafve's; it is developed separately and has releases of its
own. The fork's home is <https://gitlab.com/hmailserver/hmailserver>.

hMailServer runs on Linux in a container from the project's own image, and on
Kubernetes from the project's own Helm chart. Both are made by the release's GitLab
CI pipeline from the release's own files, and signed with Sigstore by that pipeline,
like every other file of the release:

| | Where |
|---|---|
| The image | `registry.gitlab.com/hmailserver/hmailserver:<version>`, and for a release without a suffix `:<major>.<minor>` and `:latest` too, which only move forward: one index for linux/amd64 and linux/arm64 |
| The chart | `https://updates.progressiverobot.com/helm/`, the chart `hmailserver`, at the release's version; and the release's file `hmailserver-chart-<version>.tgz` |
| The Compose file | `hmailserver/source/Server/platform/packaging/docker/docker-compose.yml` |
| How to run them | `hmailserver/source/Server/platform/packaging/README.md`, "Install and get running"; every variable the image reads is in [EnvironmentConfiguration.md](EnvironmentConfiguration.md) |

The image's address is the project's path in the group `hmailserver`, which is where
the image stays. The maintainers decided on 27 September 2026 to push the first
image only once the project was at that path, where it has been since 29 September
2026, because a container repository's
address is always its project's: an image pushed from the project's first path,
`registry.gitlab.com/progressiverobot/hmailserver`, would have changed its address
under everyone who pulled it. So no image is pushed from that path, and a release
built by hand, 6.3.4 among them, has none; nor has 6.3.5, whose pipeline has no
image jobs (Publishing, below), so the first image is 6.3.6's. The images published before GitHub
suspended the project's account on 18 September 2026 were on `ghcr.io`, which has
answered 404 since. Since 18 October 2026 the project is published on GitHub again,
under the account `RainbowWizard-coder` (<https://github.com/RainbowWizard-coder/hMailServer>),
and on that day the image was copied to `ghcr.io/rainbowwizard-coder/hmailserver`
with its cosign signatures and attestations: the tags `6.3.7`, `6.3`, `latest` and
`6.3.6`, each with the digest GitLab's registry gives it - the index
`sha256:2a5de4ebd04cd1e39f69ebeca3e905dd80217c3178132bd17a07880845ae629f` for
`6.3.7`, `6.3` and `latest`. The owner made that copy public on 20 October 2026:
since then it can be pulled without signing in, at the same digests, and
`cosign verify` of it with the identity and issuer below passes (both checked
that day). On 21 October 2026 the image was copied by hand to Docker Hub, as
`docker.io/progressiverobot/hmailserver` (`docker pull progressiverobot/hmailserver:<version>`),
by its digest: the tags `6.3.7`, `6.3` and `latest`, each that index, with both
platforms' images, and the index's cosign signature and each platform's signature
and SBOM attestation, each at the digest GitLab's registry gives it. The repository
is public, and `cosign verify` of it, and `cosign verify` and `cosign verify-attestation`
of each platform's image, with the identity and issuer below pass, without signing
in (checked that day). The
`registry-mirror` job ([Channels.md](Channels.md), "Container registries") is what
copies a release there. Nothing here pulls from either copy. The container registry is GitLab's: each
release's pipeline pushes and signs its image there, and
`registry.gitlab.com/hmailserver/hmailserver` is the address the Compose file, the
chart and these documents give.

## The image

The Dockerfile is `hmailserver/source/Server/platform/packaging/docker/Dockerfile`.
The image is the release's `.deb` installed on Ubuntu 24.04, pinned by digest,
running as the package's own user, `hmailserver`, with the one capability the mail
ports below 1024 need set on the binary rather than given to the container.

It is built for two platforms, **linux/amd64** and **linux/arm64**, each from the
release's `.deb` of its own architecture - `hmailserver_<version>_amd64.deb` and
`hmailserver_<version>_arm64.deb`, the two a release carries - and the tags name
one multi-platform index, a Docker manifest list, over the two images. `docker
pull`, Compose, Kubernetes and Podman take the tag and fetch the image of their
own platform; `docker manifest inspect registry.gitlab.com/hmailserver/hmailserver:<version>`
lists both, each with its digest. The Dockerfile takes the `.deb` of the
architecture it is built for (`TARGETARCH`, from `packages/`; `docker build
--platform` sets it by itself, kaniko does not set it from `--custom-platform`, so
`build/ci/linux-image.sh` passes it with `--build-arg`), and its base is pinned by
the digest of Ubuntu's multi-platform index, of which the builder takes that
platform's entry. The build ends by running `hmailserver --version`, so an image
whose server cannot run on its own platform, with its own libraries, is never
made.

The pipeline builds the arm64 image on a runner with the tag `linix` - linix,
since 5 October 2026 server2 or since 13 October 2026 s1-linux, all three x86-64
machines - where the kernel hands
each arm64 program to qemu-user. In 6.3.6's pipeline server2 built it and
started it, under emulation; no arm64 machine has run it yet.

| | |
|---|---|
| Ports | 25, 110, 143, 465, 587, 993, 995, 4190 and 8045 (the REST API, the Control Deck and the webmail) |
| Volumes | `/var/lib/hmailserver` (the message store), `/var/log/hmailserver`, `/etc/hmailserver` (`hMailServer.ini`) |
| Health | port 25 accepts a connection |
| Start | `docker-entrypoint.sh`: the environment becomes the configuration, the database is created or upgraded, and the server runs in the foreground |
| Kind of installation | `HM_INSTALL_KIND=container`, for the update check (Updates, below) |

The database is not in the image: point `HM_DB_*` at a PostgreSQL or MariaDB
server. The image carries the client the server needs for each: libpq, which the
server is linked against and which comes with the `.deb`, and, from the release
after 6.3.7, MariaDB Connector/C (Ubuntu's `libmariadb3`), which the server opens
at run time when `HM_DB_TYPE` is `MySQL`, the type a MariaDB server is given, and
which no `.deb` the release carries depends on, recommends or suggests (the
Debian source package the PPA builds suggests it, in its `debian/control`), so
the Dockerfile installs it by name. The build loads `libmariadb.so.3` as the
server does, and stops if it cannot. 6.3.7's image and the ones before it carry
libpq alone: they reach PostgreSQL only, and told `HM_DB_TYPE=MySQL` they stop at
the entrypoint's `--create-database` with "The MySQL client library could not be
loaded". Each platform's SBOM lists `libpq5`, and from the release after 6.3.7
`libmariadb3` and the two packages it brings, `mariadb-common` and
`mysql-common`. `HM_DB_WAIT_SECONDS` holds the entrypoint until that server
accepts a connection, for a database started beside the container with nothing
else to wait for it. The REST API is the administration on Linux, and the server
requires TLS on it anywhere but loopback, so a reachable API means a certificate
mounted in (`HM_REST_CERTIFICATE_FILE`, `HM_REST_PRIVATE_KEY_FILE`), or the
self-signed pair `hmailserver --apply-new-install-defaults` makes for a new
installation - which a container is not given by itself, since only the packaged
systemd unit's first start applies a new installation's defaults
([SecureDefaults.md](SecureDefaults.md)).

To build it yourself for your own machine's platform, put that architecture's
`.deb` in `packages/` beside the Dockerfile and run `docker build` there (BuildKit,
Docker's builder since Docker 23, sets `TARGETARCH`; with the older builder, add
`--build-arg TARGETARCH=amd64` or `arm64`). Add `--build-arg
HM_IMAGE_VERSION=<the .deb's version>` for the image's version label; without it
the label says `local` (until 6.3.6 it said Ubuntu's `24.04`, the base image's).

The image's labels, which a registry and Artifact Hub show: the OCI ones
(`org.opencontainers.image.title`, `description`, `licenses`, `vendor`, `url`,
`source`, `documentation`, and from the build `version`, `revision` and `created`,
the commit's time) and Artifact Hub's (`io.artifacthub.package.readme-url`, this
document; `license`, `keywords`, and `prerelease` for a version with a suffix).
`build/ci/linux-image.sh` fails a build without the three Artifact Hub requires. From
the release after 6.3.7 the title is "hMailServer (Progressive Robot's fork)" and the
description opens with "Progressive Robot's fork of hMailServer, not the original";
6.3.7's image, and those before it, say "hMailServer".

### Checking an image

The index the tags name is signed by its digest, and so is each platform's image;
each platform's software bill of materials (CycloneDX) is attested to that
platform's image; all of it as the release's pipeline. A tag names the index, so
`cosign verify` of the tag checks the index's signature. The platforms' images
are found by their digests, which `docker manifest inspect` of the tag lists
(`regctl image digest --platform linux/arm64 <image>:<version>` prints one). With
cosign 2.4 or later:

```
cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver:<version>

docker manifest inspect registry.gitlab.com/hmailserver/hmailserver:<version>

cosign verify \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver@sha256:<the platform's digest>

cosign verify-attestation --type cyclonedx \
  --certificate-identity https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version> \
  --certificate-oidc-issuer https://gitlab.com \
  registry.gitlab.com/hmailserver/hmailserver@sha256:<the platform's digest> | jq -r .payload | base64 -d | jq .predicate
```

Each SBOM names its image by its digest and its platform: the subject's purl
carries `arch=amd64` or `arm64`, and its `hmailserver:platform` property says
`linux/amd64` or `linux/arm64`. `container-image-sign`'s artifact,
`container-image-signed/verification.txt`, has these commands with the release's
digests filled in.

**And the project's id.** The identity is a path, and a path is only a name: were
the group or the project renamed or deleted, another project could be made at the
same path, and its pipeline could sign as that identity. The certificate also
carries the project's own id, `86730559`, which GitLab gives no other project
(Fulcio's source repository identifier, the extension `1.3.6.1.4.1.57264.1.15`),
and a server requires it at the group's path. cosign checks no GitLab claim but the
identity, so read the id from the certificates of the signatures cosign verified,
for the tag and for each platform's digest:

```
cosign download signature registry.gitlab.com/hmailserver/hmailserver:<version> \
  | jq -r 'select(.Cert != null) | .Cert.Raw' \
  | while read -r raw; do
      printf '%s' "$raw" | base64 -d | openssl x509 -inform DER -noout -text \
        | grep -A1 -e 'Subject Alternative Name' -e '1.3.6.1.4.1.57264.1.15'
    done
```

Every certificate whose Subject Alternative Name is the release's identity must
show `..86730559` under `1.3.6.1.4.1.57264.1.15` (the two dots are the value's DER
tag and length); a certificate of that identity with any other id, or none, is not
this project's. OpenSSL reads the extension from the certificate's structure, which
is what makes this a check: a search of the certificate's bytes for the id could be
fooled by a public key holding those bytes. The SBOM attestations are checked here
by their identity; the channels' registry mirror reads their certificates from the
registry and holds them to the id as well ([Channels.md](Channels.md)).

The identity is the project's path when the image was signed: an image is pushed and
signed only from the group's path, so it is the group's. The `.deb` the image was
built from may have been signed before the project moved on 29 September 2026 (6.3.5's
was), as
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
jq -r .verificationMaterial.certificate.rawBytes hmailserver-chart-<version>.tgz.cosign.bundle \
  | base64 -d | openssl x509 -inform DER -noout -text | grep -A1 1.3.6.1.4.1.57264.1.15
helm install mail ./hmailserver-chart-<version>.tgz --set database.host=...
```

The second command must print `..86730559` under `1.3.6.1.4.1.57264.1.15`: the
project's id, as for the image above. From the source tarball or a checkout,
`python3 build/ci/sigstore-certificate.py repository-id <bundle>` reads the same
extension as the server does and prints `id=86730559`.

The identity is the project's path when the release was signed, as for every
file of the release: a release tagged before the project moved into the group on
29 September 2026 (6.3.5, the one its pipeline signed there) was signed as
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
and the release page's verification section says which.

`helm pull hmailserver/hmailserver --version <version>` saves the same file: the
repository's index names each chart by its URL in the project's package registry,
the file the release page links, with its SHA-256. The chart carries no Helm
provenance file (`.prov`, a PGP signature): its signature is Sigstore's, as every
other file's is, so no key of the owner's is needed.

## Artifact Hub

Artifact Hub (artifacthub.io) indexes charts and images and hosts neither. The
owner's account, `progressiverobot89`, and the organization `hmailserver` exist;
the organization's home URL has been `https://gitlab.com/hmailserver/hmailserver`
since 6 October 2026, the day the maintainers added its two repositories, the
chart's and the image's. Each listing gets the Verified publisher label from a
metadata file that carries its repository ID, which the tree holds from that day
and which reaches Artifact Hub as each item below says. Both listings are at 6.3.7
and both have the label: Artifact Hub's API gives each repository as a verified
publisher's, the chart with the versions 6.3.7 and 6.3.6 and the image with 6.3.7,
`latest` and 6.3.6, and neither with an error at its last tracking - the chart's
on 18 October 2026, the image's on 21 October 2026 (read that day). Neither is
marked Official.

- **The chart.** The repository `hmailserver`, shown as hMailServer, of kind Helm
  charts, with the URL `https://updates.progressiverobot.com/helm/`, which has
  held the chart since 6.3.6. Its repository ID,
  `d8768ed9-b894-45c8-ba44-f7516bcf4206`, is `ARTIFACTHUB_REPOSITORY_ID` in
  `build/ci/helm-repo.py`, and `package-repos`, which runs that script after the
  pages job and deploys the site, writes it into `helm/artifacthub-repo.yml`
  beside the index, which gives the listing its Verified publisher label. The
  default branch's `package-repos` has written it since the ID landed there, and
  the file the site serves names it (read on 14 October 2026). Artifact Hub lists
  the chart at `https://artifacthub.io/packages/helm/hmailserver/hmailserver`, at
  6.3.7, with the label. Until 6.3.7 it listed 6.3.6 without it: its tracking of 6
  October 2026 at 03:45 UTC, before the ID reached the default branch, read the
  file 6.3.6's pipeline had written, which said that no ID was there yet. The chart's
  annotations name its licence (`artifacthub.io/license`), its image
  (`artifacthub.io/images`, stamped with the release's version, with the image's
  two platforms, `linux/amd64` and `linux/arm64`, which `helm-chart-package.sh`
  holds the tree's chart and the package to), its links, and whether it is a
  pre-release. The Official label is requested through a GitHub issue, and is
  not.
- **The image.** The repository `hmailserver-image`, shown as hMailServer
  container image, of kind Container images, with the address
  `oci://registry.gitlab.com/hmailserver/hmailserver`, lists it from its labels,
  which both platforms' images carry alike, at the tags the repository tracks:
  `6.3.7` and `6.3.6`, each marked immutable, which Artifact Hub reads once, and
  `latest`, marked mutable, which it reads again when the tag moves. A maintainer sets those tags
  on Artifact Hub, in its Control Panel or through its API, ten at most; a new
  release's version tag is not tracked by itself, so a maintainer adds it with
  each release. Its Verified publisher label needs a metadata file of its own,
  with the repository ID Artifact Hub gave that repository,
  `8cd87f71-a93b-4eb0-b3a0-a78ece5b5796`, pushed to the image's repository under
  the tag `artifacthub.io`. That file is
  `platform/packaging/docker/artifacthub-repo.yml` in the tree, which holds the
  ID as a line `repositoryID: <the ID>`, and the job `container-image-artifacthub`
  pushes it. That job runs in every release's pipeline after `container-image`,
  and in the default branch's a maintainer starts it by hand, so that the label
  need not wait for a release. 6.3.6's pipeline ran it while the file held no
  ID, and it pushed nothing; without an ID it pushes nothing, and passes. Its
  first push was that start, in the default branch's pipeline 2918038819 on 6
  October 2026, once the ID had landed there (job 16970526037, "ARTIFACT HUB
  METADATA PUSHED"): GitLab's registry took the artifact of Artifact Hub's
  configuration type, both of whose media types are on the registry's list
  (the container registry's `docs/supported-media-types.md`, read on 2 October
  2026), and the listing shows the Verified publisher label (Roadmap3.md,
  "Artifact Hub: the container image"). It
  pushes with `regctl` (`artifact put`), from the image `container-image` runs in,
  since ORAS comes from GitHub, in the shape Artifact Hub documents for
  `oras push`: an empty configuration of the type
  `application/vnd.cncf.artifacthub.config.v1+yaml`, and the file as the one layer,
  of the type `application/vnd.cncf.artifacthub.repository-metadata.layer.v1.yaml`,
  titled `artifacthub-repo.yml`. It reads the tag back and holds the layer to the
  file byte for byte (`build/ci/container-image-artifacthub.sh`). The registry's
  cleanup policy, still switched off, keeps that tag by name since 6 October
  2026, when the maintainers added `^artifacthub\.io$` to the tags it keeps
  through the project's API (ContinuousIntegration.md, "What the owner sets on
  the registry"). Nor does the
  registry's protected-tag rule for the release's tags cover it, and the file
  under that tag is also what Artifact Hub checks an ownership claim of the
  listing against (its `owners`): a second rule protects `^artifacthub\.io$` as
  the release's tags are protected, push Maintainer and delete Owner (the same
  place), which the maintainers added through the project's API on 2 October
  2026, before the repository was added. Should somebody else have added the
  repository first, the listing is taken back by such a claim, which needs an
  `owners` list naming the claimant in the file; the script refuses that key,
  so allowing it would be a change to the script, reviewed as such.

## Updates

The server does not update itself in a container: a new release is a new image. From
6.3.6 the image sets `HM_INSTALL_KIND=container`, and the server's update check,
finding a newer release, says so and names the image to pull, and downloads and
installs nothing ([UpdateFeed.md](UpdateFeed.md)). Then:

- Compose: set the new version on the `image:` line, `docker compose pull`,
  `docker compose up -d`; the entrypoint upgrades the database at the start. The
  file's own PostgreSQL moved from 16 to 18 in 6.3.6: see below.
- Helm: `helm repo update`, then `helm upgrade mail hmailserver/hmailserver
  --reuse-values`.

### PostgreSQL 16 to 18, in the Compose file

From 6.3.6 the Compose file's database is PostgreSQL 18, with its volume `hm-db`
mounted at `/var/lib/postgresql`: the `postgres:18` image keeps each major version's
data in a directory of its own there (`18/docker`), so that a later move can be made
beside the old data. Until 6.3.5 it was PostgreSQL 16, with `hm-db` mounted at
`/var/lib/postgresql/data`. A deployment that keeps its own copy of the file keeps
PostgreSQL 16, on which 6.3.6 runs as before (PostgreSQL supports 16 until November
2028); what follows is for taking the file's PostgreSQL 18.

PostgreSQL 18 does not open PostgreSQL 16's data. On a deployment made from an
earlier copy of the file, the new file's `db` stops as it starts - `docker compose
logs db` begins `Error: in 18+, these Docker images are configured to store database
data in a format which is compatible with "pg_ctlcluster"` - and `hmailserver`, which
waits for a healthy database, is not started. PostgreSQL 16's data is not changed:
the failed start leaves only an empty directory, `18/docker`, beside it, which 16
ignores and step 2 below moves aside with the rest, and putting the old file back
starts the deployment as it was.

The database is moved once, by a dump taken on 16 and loaded into 18, with nothing
but PostgreSQL's own images. In the directory of the Compose file:

```
# 1. On the old file (postgres:16): stop the server, dump its database, stop the rest.
#    The dump holds every account and its password hash: it is made readable by you
#    alone (umask 077), and removed at the end.
docker compose stop hmailserver
(umask 077; docker compose exec -T db pg_dump -U hmailserver -d hmailserver > hmailserver-pg16.sql)
# A whole dump ends with this line: 1 here, or the dump failed - go no further
# (docker compose start hmailserver brings the server back on 16).
grep -c '^-- PostgreSQL database dump complete' hmailserver-pg16.sql
docker compose down

# 2. Take the new file (postgres:18, hm-db at /var/lib/postgresql). Move PostgreSQL
#    16's files aside in the volume, into pg16/, where 18 does not look.
docker compose run --rm --no-deps --entrypoint bash db -c \
  'cd /var/lib/postgresql && mkdir pg16 && find . -mindepth 1 -maxdepth 1 ! -name pg16 -exec mv -t pg16 {} +'

# 3. Start the new database alone, load the dump into it, then start the server.
docker compose up -d --wait db
docker compose exec -T db psql -U hmailserver -d hmailserver -v ON_ERROR_STOP=1 -q < hmailserver-pg16.sql
docker compose up -d

# 4. Once the server is as it was: remove PostgreSQL 16's files and the dump.
docker compose exec db rm -rf /var/lib/postgresql/pg16
rm hmailserver-pg16.sql
```

`psql` prints a line or two for each sequence the dump sets (`setval`); it stops at
the first error with an exit status other than 0. The way back, at any point before
step 4: `docker compose down`, then, still on the new file,

```
docker compose run --rm --no-deps --entrypoint bash db -c \
  'cd /var/lib/postgresql && rm -rf 18 && find pg16 -mindepth 1 -maxdepth 1 -exec mv -t . {} + && rmdir pg16'
```

and the old file, `docker compose up -d`. The way back returns the database to the
moment of the dump: mail delivered and changes made on 18 since are in 18's database
alone, which it removes, and the files of those messages stay in `hm-data` with no
row in 16's. The messages themselves are files in `hm-data`, which none of these
steps touches.

On 2 October 2026, on linix (Docker 29.1.3, Compose 2.40.3), with an image built
from the Dockerfile and 6.3.6's `.deb`: a deployment on the old file, PostgreSQL
16.15, a domain made over the REST API; the new file without the move - `db`
restarting with that error, `hmailserver` created and never started, `PG_VERSION`
still 16 at the volume's top; the four steps - a dump of 131 tables, the restore's
exit status 0, PostgreSQL 18.6 with its data in `/var/lib/postgresql/18/docker`,
schema 6105, the domain answered by the REST API and a new one made, and the database
restarted after step 4 with both; the way back - the server on 16.15 again with the
domain made before the move; and a new deployment on the new file - 18.6, the schema
created, a domain made. The same day, with PostgreSQL's two images alone: after the
new file's failed start, which left the empty `18/docker` at the volume's top, the
old file put back started 16.15 with its data; and a volume taken through that
failed start, step 2 and 18's first start, then the way back, did too. Step 1's
check found its line in a whole dump from 16.15, and not in a dump of a database
that does not exist (empty, pg_dump's exit status 1) or in one cut short.

## How the pipeline makes them

### The image jobs

`.gitlab/ci/container-image.yml`, in every protected `v*` tag's pipeline, after
`release-publish`:

| Job | Where | What it does |
|---|---|---|
| `container-image-deb` | a runner with the tag `linix`, in cosign's image (`HM_COSIGN_IMAGE`); **started by hand** | Takes `hmailserver_<version>_amd64.deb`, `hmailserver_<version>_arm64.deb` and their SBOMs from the published release, with their Sigstore bundles, and verifies each with `cosign verify-blob` as signed by this project's pipeline for the tag, at either of the project's two paths, with the project's id `86730559` required in the certificate's extensions at the group's path, read from the v0.3 bundle cosign verified by `build/ci/sigstore-certificate.sh` (`build/ci/container-image-deb.sh`). Since 6.3.6 it also takes the release's `hmailserver-linux-<version>-SHA256SUMS.txt`, verified the same way, and holds each `.deb` to the SHA-256 it gives that name and each SBOM to naming its `.deb`'s SHA-256: a signature binds bytes, not a name, so another file the pipeline signed, served under a `.deb`'s name, would verify. A release without either `.deb`, or its SBOM, stops it: the image is built for both platforms or not at all |
| `container-image-amd64`, `container-image-arm64` | a runner with the tag `linix` (linix, server2 or s1-linux), in kaniko's image | Each builds the Dockerfile for its platform from its own architecture's `.deb` - the arm64 one under qemu-user - checks the image's platform, user, entrypoint, healthcheck, ports, file capability, `HM_INSTALL_KIND` and labels and that the server answered `--version` in the build, and keeps the image's tarball for `container-image`; neither pushes (`build/ci/linux-image.sh`, `HM_IMAGE_RELEASE=1`) |
| `container-image` | a runner with the tag `linix`, in regctl's image | Holds each tarball to what its job checked, joins the two images under one index, checks that the index names exactly linux/amd64 and linux/arm64, and only then pushes: each image by its digest, the index as `:<version>` (and `:<major>.<minor>` and `:latest` for a release without a suffix, each only forward: see below). It reads every tag it wrote back (`build/ci/container-image-push.sh`) |
| `container-image-artifacthub` | a runner with the tag `linix`, in regctl's image; in the default branch's pipeline, **started by hand** | Pushes the tree's `platform/packaging/docker/artifacthub-repo.yml` under the tag `artifacthub.io`, as Artifact Hub reads it, when it holds the repository ID, as it does since 6 October 2026, and reads it back; for a file without one, nothing ([Artifact Hub](#artifact-hub)). Nothing waits for it (`build/ci/container-image-artifacthub.sh`) |
| `container-image-sbom` | a runner with the tag `linix`, in the Linux CI image | For each platform, Syft's reading of that platform's pushed image by its digest, with what that architecture's `.deb`'s own SBOM names inside the binary (`build/ci/container-image-sbom.sh`) |
| `container-image-sign` | a runner with the tag `linix`, in cosign's image, after both start tests (since 6.3.6) | Signs the index and each platform's image by its digest, attests each platform's SBOM to its image, and verifies all of it (`build/ci/container-image-sign.sh`): only an image that has started and answered is signed |
| `container_scanning`, `container_scanning_arm64` | a runner with the tag `linix`, in Trivy's image | Trivy over each platform's pushed image by its digest, with that day's vulnerability database (`build/ci/container-scan.sh`); a finding is a warning, and its report, in GitLab's container-scanning format, is the job's artifact. GitLab's own Container Scanning template ran until 10 October 2026, when its analyzer's pinned digest was gone from GitLab's registry (ContinuousIntegration.md, "container_scanning's scanner") |
| `container-image-start`, `container-image-start-arm64` | a runner with the tag `linix`, in the Linux CI image | Starts each platform's pushed image by its digest twice as services, once beside PostgreSQL and once beside MariaDB - the arm64 one as services of that platform, under qemu-user - asks each for SMTP on 25 and for the REST API over TLS, and reads from each database the schema the server laid out in it (The start test, below) |

In a branch's pipeline, `linux-image` builds the linux/amd64 image from
`linux-build`'s `.deb` and `linux-image-arm64` the linux/arm64 image from
`deb-ubuntu26.04-arm64`'s, each checked as above and pushed nowhere.

In 6.3.6's tag pipeline 2914690858 server2 ran `container-image-amd64`,
`container-image-arm64`, `container-image-sbom` and `container-image-start-arm64`,
and linix `container-image-start` (ContinuousIntegration.md, "The runners").

**Why two build jobs and regctl.** kaniko, which builds without a Docker daemon or
privilege, builds one platform a run and cannot join two images under one index.
So each platform has its own kaniko job, and `container-image` joins and pushes
them with regctl, from Docker Hub's `regclient/regctl`, pinned by digest: the same
release the channels' registry mirror takes (`HM_REGCTL_IMAGE`,
`.gitlab/ci/channels.yml`), in its `-alpine` variant, which has the shell a job
needs, and the tool this document already named for Artifact Hub's metadata.
regctl imports each tarball into an OCI image layout of the job's own and makes
the index there, so the index is checked before anything is sent. crane's `index
append` joins images already in a registry, so each image would be sent before
the index could be checked; `manifest-tool`'s releases are GitHub's, which this
project takes nothing from.

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
run**, not when the pipeline is made: `container-image-deb`, `linux-image.sh` (for a
release) and `container-image-push.sh` refuse, saying so, with exit status 3, while
it is anything but `1`. So the jobs are in every `v*` tag's pipeline whatever the
variable was at the tag, and a release tagged before the variable is set has its
image pushed from its own pipeline after it. A tag's pipeline runs the tag's own CI
files: 6.3.5's made its image jobs only if the variable was set when its pipeline
was made, it was not, and 6.3.5's pipeline (2892097590) has none. So 6.3.5 has no
image, and the first is 6.3.6's, for both platforms:

1. The project is at `gitlab.com/hmailserver/hmailserver`, since 29 September
   2026 (Roadmap3.md, section 8, the move into the group), and the registry's
   cleanup policy keeps the eight documented tag patterns (ContinuousIntegration.md, "What the owner sets on the
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
`container-image` pushes only at the group's path, and at any other it fails and
pushes nothing; `container-image-amd64` and `container-image-arm64` build nothing for
another: the project's first path, `registry.gitlab.com/progressiverobot/hmailserver`,
is another project's since the move.
`container-image-sbom` and `container-image-start` run in the Linux CI image of the
tag's commit, `.linix`'s, and start, as every job in it does, with
`build/check-ci-image.py --job`. Since 6 October 2026 `.linix` names that image in the
project's registry by digest
(`registry.gitlab.com/hmailserver/hmailserver/ci-linux:<release>-<hash>@sha256:<digest>`),
and whichever runner with the tag `linix` takes the job (linix, server2 since 5
October 2026 or s1-linux since 13 October 2026) pulls it from there: keep it in the registry until the release's image
jobs have run ([ContinuousIntegration.md](ContinuousIntegration.md), The image). The
registry's cleanup policy, were it switched on, would keep its tag by name.
`container-image-deb` is not allowed to fail, because GitLab starts a job whose
need was allowed to fail after that need has failed: the image would be built after
a refusal, or after a bundle that did not verify. So until someone starts it, the
tag's pipeline shows as blocked on it - the release's image is still to be pushed -
and a refusal fails it; running it again once the variable is set lets the jobs
after it go on.

**`:latest` and `:<major>.<minor>` only move forward.** The image jobs of a release
can run after a newer release's - started late, since they wait for a press, or an
old pipeline's `container-image` run again - and they still push `:<version>`, but
never take the other two back. Before it moves either, `container-image` reads what
the tag names now: it lists the repository's tags, and reads the version label
(`org.opencontainers.image.version`) of that image's linux/amd64 image. It makes the
tag when there is none, moves it when that image is this version (a re-run of the
release) or an older one, compared number by number (6.3.10 is newer than 6.3.9),
and leaves it where it is when that image is a newer version or its version cannot
be read (no label, one that is not `<major>.<minor>.<patch>`, or the tags cannot be
listed). A tag left where it is makes a `NOT MOVED` line in the job's log and is
named in `container-image/tags.txt` with the reason; the job still passes, so that
the release's own image is signed, scanned and started. If it should have moved - the
image it names is one put there by hand, without the label - a maintainer copies the
index to it by hand (`regctl image copy <image>@<the index's digest> <image>:latest`).
So 6.3.12 pushed after 6.4.0 moves `:6.3` and leaves `:latest` on 6.4.0.

### When a job fails

Nothing is pushed until both platforms' images are built and checked and the index
naming them is made and checked, and no tag is written until both images and the
index are in the registry. A build job that fails leaves the registry as it was:
fix it and run the jobs again. If `container-image` stops between the push and the
tags, the images it pushed by digest are reached by no tag, and nothing a user pulls
has changed; running `container-image` again pushes the same digests and writes
the tags.

`container-image` reads its tags back after it pushes them, and
`container-image-start` and `container-image-start-arm64` run after the push. If
one fails, the tags name an image that did not pass: the job's log names them, and
they are deleted by hand (Deploy, Container registry). `container-image-sign`
waits for both start tests, so such an image is never signed (until 6.3.6 it was
signed beside them, and its `sha256-<digest>.sig` and `.att` tags were deleted with
it). Then the fix is released as a new version; a tag is spent once.

### The start test

`build/ci/container-image-start-test.sh` starts the image as a user starts it,
twice - once beside PostgreSQL and once beside MariaDB, told `HM_DB_TYPE=MySQL` -
through its own entrypoint, with the database, the administrator password, the
REST listener and `HM_DB_WAIT_SECONDS` from the environment, and the command
`hmailserver --apply-new-install-defaults && exec hmailserver --foreground`: the
new installation is secured - a self-signed certificate, STARTTLS - before the
server starts, and the REST listener is pointed at that certificate, so the test
needs no certificate from outside. It then asks each of the two:

- port 25: the banner, `EHLO` answered with `STARTTLS` among the extensions;
- the REST API on 8045 over TLS: `GET /api/v1/status` refused with 401 without
  credentials, answered with the release's version as Administrator, and not
  answered over plain HTTP;
- its database: `hm_dbversion`, read with that database's own client, holds the
  schema version the server laid out there, and the two databases hold the same.

In the job, the runner starts PostgreSQL (`db`), MariaDB (`mariadb`, the image
`linux-db-upgrade-mysql` runs, by digest, and as it runs it, with root and no
password: `MARIADB_RANDOM_ROOT_PASSWORD` would print the password it made into
the job's log) and the image twice (`mail` and `mail-mariadb`, each given its
database's type and address in its own service's variables) as the job's
services on a network of the job's own (`FF_NETWORK_PER_BUILD`); the test reads
the databases with the Linux CI image's `psql` and `mariadb`, and
`CI_DEBUG_SERVICES` puts the services' output in the job's log. An image without
the MariaDB client fails the second start: its server stops at
`--create-database`, and port 25 never answers.

`container-image-start-arm64` does the same with the linux/arm64 image, named by
its own digest (the dotenv report's `HM_IMAGE_ARM64`), as a service of the platform
linux/arm64 (`services:docker:platform`), which the runner runs under qemu-user, and
waits up to ten minutes for it to answer. Under emulation the kernel does not apply
the binary's file capability to the emulator it runs; the container binds port 25
all the same, because Docker lets a container's own processes bind any port
(`net.ipv4.ip_unprivileged_port_start=0` in its network namespace). The capability
itself is checked in both images' layers by the build jobs, and is used as it is on
an arm64 machine.

**By hand**, on any machine with a Docker daemon - linix, or a laptop's WSL - the
script starts the same four containers itself, reads each database with its own
container's client, and removes them afterwards (`HM_START_NAMES` names them and
their network; `hm-start-<process id>` by default):

```
# an image from a registry, or one built from the Dockerfile with a .deb in packages/
HM_START_VERSION=<version> bash build/ci/container-image-start-test.sh --docker <image>

# the image linux-image built without pushing, from its tarball (HM_KEEP_IMAGE=1)
docker load -i linux-image/hmailserver-image.tar
HM_START_VERSION=<version> bash build/ci/container-image-start-test.sh --docker hmailserver:<version>-amd64

# the arm64 one, on an x86-64 machine with qemu-user's binfmt handler for aarch64
docker load -i linux-image-arm64/hmailserver-image.tar
HM_START_PLATFORM=linux/arm64 HM_START_VERSION=<version> \
  bash build/ci/container-image-start-test.sh --docker hmailserver:<version>-arm64
```

`--keep` leaves the containers running and says how to remove them. On 28
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
tags to delete. Both were run beside PostgreSQL alone, as the test was then.

On 21 October 2026 the Dockerfile of this tree, with `libmariadb3`, was built from
6.3.7's published amd64 and arm64 `.deb` files (each held to the release's
`hmailserver-linux-6.3.7-SHA256SUMS.txt`) on an x86-64 Ubuntu machine with Docker
29: the amd64 image with Docker's builder and both with `build/ci/linux-image.sh`
in kaniko's image, the arm64 one under qemu-user, each ending "IMAGE BUILT". The
start test then passed by hand against the amd64 image and, with
`HM_START_PLATFORM=linux/arm64`, the arm64 one, and in its job mode with the four
services and the Linux CI image standing in for the runner: beside PostgreSQL 18
and beside MariaDB 13 each server created its schema, 6108, from
`CreateTablesPGSQL.sql` and `CreateTablesMySQL.sql`, and answered SMTP with
STARTTLS and the REST API over TLS as `6.3.7-B46`. The image built the same way
from the Dockerfile before that change passed beside PostgreSQL and failed beside
MariaDB, its server stopping at `--create-database` with "The MySQL client library
could not be loaded ... Tried: libmariadb.so.3, libmysqlclient.so.21,
libmysqlclient.so, libmariadb.so"; and the Dockerfile with the package left out
of its `apt-get install` stopped at its build, where the library is loaded. No
tag's pipeline has run the two starts yet - two services of one image, each
with variables of its own - and the next release's is the first.

### What was run by hand

On linix on 29 September 2026, beside batch42's pipeline, from 6.3.5's published
amd64 `.deb` (held to the release's `hmailserver-linux-6.3.5-SHA256SUMS.txt`) and the
arm64 `.deb` of batch41's pipeline 2887524317 (`deb-ubuntu26.04-arm64`, job
16766310075, version 6.3.5, SHA-256 `41967a00...`), with this tree's Dockerfile and
scripts, in the jobs' own images, unprivileged, as the jobs run them:

- **Both platforms built.** `linux-image.sh` built the linux/arm64 image in 162 to
  217 s, its `RUN` under qemu-user through linix's qemu-aarch64 handler (flags POF),
  and the linux/amd64 one in 44 to 50 s, in branch mode and in release mode
  (`HM_IMAGE_RELEASE=1`, as `container-image-amd64` and `-arm64`). Each answered
  `hMailServer 6.3.5 build 44` in its build, and each passed every check: the arm64
  image's configuration says `arm64`, variant `v8`, and its binary kept
  `cap_net_bind_service`. Each tarball is about 85 MB.
- **The arm64 image started under emulation.** Loaded into Docker and run as
  linux/arm64, it said `aarch64` and `hMailServer 6.3.5 build 44`, ran as
  `hmailserver`, and `getcap` showed the capability; the start test beside
  PostgreSQL passed in 19 s (SMTP with STARTTLS, the REST API over TLS as
  `6.3.5-B44`, 401 without credentials, nothing over plain HTTP). The amd64 image's
  passed in 9 s. In the arm64 container the server's process was `qemu-aarch64` with
  no effective capability, where the amd64 one's had `cap_net_bind_service`, and it
  bound port 25 all the same: Docker set `net.ipv4.ip_unprivileged_port_start` to 0
  in both containers (1024 on linix itself).
- **The push, against a stand-in.** `container-image-push.sh`, in the job's regctl
  image, pushed both release builds to a registry answering as
  `registry.gitlab.com` over TLS (distribution's `registry:3`, a throwaway
  certificate, on a docker network of its own): a Docker manifest list naming
  linux/amd64 and linux/arm64 (variant `v8`) as `6.3.5`, `6.3` and `latest`, and no
  other tag. Ten refusals stopped it before anything was sent: the switch unset or
  0, the project's first path, a branch's pipeline, another tag's version, no arm64
  image, the amd64 image in the arm64 job's place, a tarball changed after its check,
  an `image.json` that was not the tarball's configuration, and no registry
  password. `linux-image.sh`'s release mode refused, before kaniko ran, the switch
  unset or 0, the first path, a branch's pipeline, a missing `.deb` and a platform
  not built.
- **The SBOMs.** `container-image-sbom.sh`, in the Linux CI image, read both images
  from the stand-in by digest and made one document each (4,398 components; the
  subject's purl with `arch=amd64` or `arch=arm64`), from 6.3.5's published amd64
  `.deb` SBOM and, for arm64, an SBOM `build/ci/packages/package-sbom.sh` made from a
  record written by hand from that job's `build-info.txt` (batch41 had no arm64 SBOM
  job yet). Syft found the `hmailserver` package of each architecture.
- **Signing, with a throwaway key.** cosign 2.6.5, in `HM_COSIGN_IMAGE`, with a key
  and no transparency log in place of Fulcio and Rekor, signed the index and both
  images by digest and attested each platform's SBOM to its image; each verified,
  and `cosign verify` of the tag `6.3.5` found the index's signature. The registry
  then held `.sig` tags for all three digests and `.att` tags for the two images.
- **Scanning.** GitLab's container-scanning analyzer, version 8 (gcs 8.6.35, Trivy
  0.72.0), scanned each platform's image by digest in 6 s: 18 findings each, and its
  SBOM listed the `hmailserver` package of that architecture. Its jobs run Trivy
  0.74.0 instead since 10 October 2026 (`build/ci/container-scan.sh`), which, as the
  jobs run it, scanned 6.3.6's two published images by digest on server2 that day
  in 18 s each: 19 findings each, each report naming its platform.
- **The tags only forward.** Against the same kind of stand-in, as a user that neither
  owns the checkout nor is root, `container-image-push.sh` pushed small images of
  both platforms, each labelled with its version and built with kaniko as the jobs
  build them, in this order:
  - 6.3.5 into an empty repository made `:6.3` and `:latest`;
  - 6.3.6 moved both;
  - 6.3.5 again, rebuilt, pushed `:6.3.5` and moved neither;
  - 6.3.6 again, rebuilt, moved both to its new index;
  - after 6.3.10, 6.3.9 moved neither;
  - with `:latest` put by hand on an image without the label, 6.3.11 moved `:6.3` and
    left `:latest`, saying so;
  - 6.4.0 made `:6.4` and moved `:latest`, and 6.3.12 after it moved `:6.3` alone;
  - 6.3.13-rc1 wrote its own tag only.
- **Artifact Hub's metadata, against a stand-in.** On 2 October 2026, against the
  same kind of stand-in, `container-image-artifacthub.sh` ran in the job's regctl
  image as the image's own user. With the tree's file, which held no ID then, it
  pushed nothing and passed. With an ID added it pushed `:artifacthub.io` and
  nothing else: an OCI image manifest whose configuration was the empty one of
  Artifact Hub's type and whose one layer was the file, of Artifact Hub's type,
  titled `artifacthub-repo.yml`; the same file pushed again, from the default
  branch, made the same manifest, and another ID replaced it. A reader written
  apart from regctl (Python, the registry's HTTP API) did what Artifact Hub
  documents - the tag's manifest, its layer of that type, the layer read as YAML -
  and found the ID. Fourteen refusals stopped it before anything was sent: the
  switch unset or 0, the project's first path, an unprotected ref, a tag not
  `v*`, a branch other than the default, neither, no registry password, an ID in
  upper case or followed by more, another key (`owners:`), two IDs, CRLF line
  endings and no file. Its read-back failed a push whose layer was another type, a
  configuration regctl writes by itself (`{}`), and a layer without its title.

### Packaging and serving the chart

`.gitlab/ci/helm-chart.yml`'s `helm-chart-package` job (linix, server2 or s1-linux,
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
pulled byte for byte and rendered with the release's image. On 2 October 2026, with
the image's platforms in `artifacthub.io/images`: the chart linted and rendered as
`chart-and-compose` asks, `helm-repo.py --self-test` passed, and
`helm-chart-package.sh` packaged it for a branch, `v6.3.6` and `v6.3.6-rc1`, `helm
show chart` printing both platforms under the release's image; it refused a
`Chart.yaml` without `linux/arm64`, with a third platform, with `linux/amd64`
twice, with a second image and without the platforms (this file's chart before),
and a package whose platforms had changed after the tree's were checked.

## What is not shown yet

- The pipeline has made two releases' images. 6.3.6's tag pipeline 2914690858 made
  the first on 6 October 2026, where every image job passed (`container-image-deb`
  on its second run), and 6.3.7's tag pipeline 2958958332 the second on 17 October
  2026, where every image job passed at its first run: in each, both platforms'
  images built, the arm64 one under qemu-user, pushed under one Docker manifest
  list, signed, attested, scanned and started, the start test's GitLab half for
  both platforms among them. Read without credentials on 21 October 2026, the
  registry's `6.3.7`, `6.3` and `latest` name 6.3.7's list,
  `sha256:2a5de4ebd04cd1e39f69ebeca3e905dd80217c3178132bd17a07880845ae629f`, and
  `6.3.6` names 6.3.6's,
  `sha256:0c7320b77209c0ce8eb032272171d60c07ea75bb05e9c4c8a28fb159ade13e8a`, each
  for linux/amd64 and linux/arm64/v8, beside cosign's signatures of each list and
  of its two images and the attestations of the images. By hand, without signing
  in, `cosign verify` has passed against the copy of 6.3.7's index on `ghcr.io`, and
  `cosign verify` and `cosign verify-attestation` against the copy on Docker Hub
  and its platforms' images, at the same digests (above).
  What *Done when* asks beyond that - `docker compose up` and `helm install` from
  the published image - is not recorded yet (Roadmap3.md, section 1b).
- No arm64 machine has run the image: only qemu-user on the project's x86-64
  runners.
- Both listings on Artifact Hub are at 6.3.7 and carry the Verified publisher
  label (read through its API on 21 October 2026): the chart with the versions
  6.3.7 and 6.3.6, last tracked on 18 October 2026, and the image with 6.3.7,
  `latest` and 6.3.6, last tracked on 21 October 2026, neither with a tracking
  error (*Artifact Hub*, above). The image's listing shows `latest` as its
  default version, beside 6.3.7. Neither listing's description says yet that it
  is Progressive Robot's fork: each is the text the release carried, the chart's
  `Chart.yaml` and the image's description label. Nor does either README that
  Artifact Hub shows. The chart's is the 6.3.7 chart's own README.md. The image's
  is this document as Artifact Hub read it when it last processed each tag, not
  master's current copy: for 6.3.7 and 6.3.6 the text of that release's tag, and
  for `latest` master's text of 19 to 21 October 2026. Artifact Hub's API, read at
  02:28 UTC on 22 October 2026, gives every version's README without this
  document's opening, though master has had it since 21 October and the image's
  repository was tracked again at 02:17 UTC that day. The descriptions and the
  READMEs say so when Artifact Hub next processes a tag, with the next release.
