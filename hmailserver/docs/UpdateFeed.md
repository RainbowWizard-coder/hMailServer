The update feed
===============

An installed server finds out whether a newer release exists by reading a
release feed (`UpdateCheckEnabled`, the Control Panel's Status page, `POST
/api/v1/update/check`). Until 18 September 2026 that feed was GitHub's Releases
API for this project. GitHub suspended the project's account that day, and the
API has answered 404 since, so every server's check has failed with "The update
feed answered HTTP 404." Nothing else breaks: the check reports and the server
runs on.

The feed is now **https://updates.progressiverobot.com**. GitLab Pages serves it
from this project, and it is made from the project's releases on GitLab. A
server built after 6.3.3 reads it without being told. A server from 6.2.28 to
6.3.3 still reads GitHub, and one line in its configuration points it here.

*Written on 25 September 2026, before the site's first deployment and before
any release has been signed on GitLab. Until the first deployment and GitLab's
certificate for the name, the address answers nothing a server can use.*

A server from 6.2.28 to 6.3.3
-----------------------------

### The line

In the `[Settings]` section of `hMailServer.ini`:

```ini
[Settings]
UpdateFeedUrl=https://updates.progressiverobot.com/releases.json
```

Use exactly that value. It is the list of every release, and one configured
feed serves both channels: a server on the stable channel takes the newest
release that is not a pre-release from the list, and one on the pre-release
channel (`UpdateChannel=prerelease`) takes the newest of all. The server builds
the address it looks a single release up at from the same value, the part up to
`/releases` followed by `/tags/v<version>`. That address is the rollback's: before
an install, the server fetches the version it is running, so that it can put it
back if the new one does not start.

Two values that look right are not:

- `https://updates.progressiverobot.com/releases`, without `.json`, is a
  directory on the site, and GitLab Pages answers it with a redirect to
  `/releases/`. The server follows no redirect when it reads the feed, so the
  check fails with "The update feed answered HTTP 302." The same goes for
  `/releases?per_page=10`, the address GitHub's list had.
- `https://updates.progressiverobot.com/releases/latest` works, but it is one
  release, the newest stable one, so a server on the pre-release channel would
  never see a pre-release.

### Setting it on Windows

Either way, the service has to be restarted before the server reads it.

- **In the Control Panel**: Settings, Network, API & monitoring, the Updates
  card. Put the value in the field labelled "Feed URL (empty = this project's
  GitHub releases; set for a mirror)", as the Control Panel up to 6.3.3 labels
  it (since 6.3.4 it is "Update feed URL (empty = the built-in feed; set for a
  mirror)"). Save, and let the Control Panel restart
  the service when it offers to.
- **In the file**: `Bin\hMailServer.INI` in the installation folder
  (`C:\Program Files\hMailServer\Bin` unless it was installed somewhere else).
  Edit it as an administrator. Add the line under `[Settings]`, or replace an
  `UpdateFeedUrl=` line that is already there. Then restart the hMailServer
  service: in Services, or with `net stop hMailServer` and `net start
  hMailServer` from an elevated prompt.

### Setting it on Linux (6.3.0 to 6.3.3)

The packages keep the configuration in `/etc/hmailserver/hMailServer.ini`.

```sh
sudoedit /etc/hmailserver/hMailServer.ini     # add the line under [Settings]
sudo systemctl restart hmailserver
```

The AppImage keeps its configuration in the home of the user who runs it:
`$HMAILSERVER_HOME/hMailServer.ini` where that is set, and otherwise
`$XDG_DATA_HOME/hmailserver/hMailServer.ini`, which is normally
`~/.local/share/hmailserver/hMailServer.ini`. Add the line there, and start the
AppImage again.

### Checking that it worked

Run a check: Check for updates on the Control Panel's Status page, `POST
/api/v1/update/check` where the REST API is on, or `Status.CheckForUpdate()`
over COM. The check reads the feed then, whether or not the daily check is on.

- On 6.3.3 the check finds the server up to date, and the application log has
  the line "Update check: this server runs 6.3.3, which is the latest release
  on the stable channel (6.3.3)."
- On an older version the answer is that a newer release is available, with
  its version and date and a link to its page on
  `gitlab.com/Progressiverobot/hmailserver/-/releases/`. `GET /api/v1/update`
  shows the installer's address in the project's package registry
  (`https://gitlab.com/api/v4/projects/86730559/packages/generic/...`).
- Before the change, the same check said "The update feed answered HTTP 404."
  If it still does, the server has not read the new value: check the section
  and the spelling, and that the service was restarted.
- "The update feed answered HTTP 302." means the value lacks `.json`.
- "The update feed could not be read: ..." names what stopped the request: no
  route out, a proxy (`HttpProxy`), or a certificate the machine does not trust.

What those servers can install through it
-----------------------------------------

The feed chooses which release a server is offered and which file it
downloads. The signature proves only who made that file. A server verifies
every installer against the Sigstore bundle published beside it: that the file
is the one signed, that the transparency log recorded the signature, and that
the signing certificate names a signer compiled into the server. A server from
6.2.28 to 6.3.3 has one such signer, the GitHub Actions workflow
`sign-release.yml` of this project, and holds a file to nothing more: it would
install any file that workflow ever signed, under whatever version the feed
gave it. Pointing it at this feed changes where it looks, not whom it trusts.
A server built after 6.3.3 checks more (see [What a server built after 6.3.3
checks](#what-a-server-built-after-633-checks)).

- **The releases up to 6.3.3 install through it.** GitLab hosts them byte for
  byte as GitHub published them, with the bundles GitHub Actions made. They
  verify against the old server's own signer, so a server on 6.2.28 can go to
  6.3.3 by the live update as before, with a rollback image.
- **The first release signed on GitLab will not install through an old
  server's live update.** The releases after 6.3.4 are to be signed by this
  project's GitLab CI pipeline for their `v*` tag, since nothing can be signed
  on GitHub any more; none has been yet. 6.3.4 itself was built and
  Authenticode-signed by hand and carries no Sigstore bundle, so no server
  installs it by the live update: one offered it says it cannot be verified and
  downloads nothing (`UpdateDownloader.cpp`; the feed's check lets it through
  by name, `UNSIGNED_BY_DESIGN` in `build/generate-update-feed.py`). It is
  installed by hand, as below. An old server's check will find a release
  signed on GitLab, but its download will be refused: "The certificate names
  https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v...,
  not this repository's release workflow." Nothing is installed, and the server
  runs on as it was.
- **Turn automatic download off until then.** With `UpdateAutoDownload=1`
  ("Download and verify the installer as soon as a release is found" on the
  Updates card; off by default) such a server downloads the whole installer,
  about 80 MB, at every check and refuses it every time: the verification comes
  after the download. Set it to 0, and set it back after the upgrade by hand.
- **That one upgrade is done by hand**, as upgrades were before 6.2.28. Take
  the backup [Upgrading.md](Upgrading.md) describes. On Windows, download
  `hMailServer-<version>-x64.exe` from the release's page and run it. On Linux,
  install the release's `.deb` or `.rpm` with `apt` or `dnf`, or replace the
  AppImage.
- **After it, the new version trusts both signers** and reads this feed without
  being told. The `UpdateFeedUrl` line can go, although leaving it changes
  nothing, since it names the same feed. Since 6.3.4 the
  `[Settings]` values are kept in the database, and a line edited in the file
  is put back (`HM5804`), so clear the value in the Control Panel, with `DELETE
  /api/v1/settings/ini/UpdateFeedUrl` over REST or, on Linux, with `hmailserver
  --set-setting UpdateFeedUrl=`, not in the file.

### Having the live update make that upgrade anyway

On Windows only - the Linux builds from 6.3.0 to 6.3.3 cannot apply an update
at all, and answer "This build cannot apply an update". A server from 6.2.28 to
6.3.3 already reads three settings that replace the identity it trusts. They
were made for a site that signs its own releases. Set to this project's GitLab
signer, they let the old server's live update install a GitLab-signed release:

```ini
[Settings]
UpdateSigningIdentity=https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v
UpdateSigningIssuer=https://gitlab.com
UpdateSourceRepository=https://gitlab.com/Progressiverobot/hmailserver
```

This has costs. While the lines are set, the server refuses every GitHub-signed
release, and that includes the rollback image of the version it is running, so
the install runs without one. The log then says "no rollback image ... could be
had", and a new version that does not start needs reinstalling by hand. These
values are what the new version compiles in, and they are proved only when the
first release signed on GitLab exists: check them against that release's notes
before you rely on them.

Leave the lines in place until the new version is running: the old server
verifies the installer once more as it hands it over, with the same settings.
Then clear all three the way the last bullet above clears `UpdateFeedUrl` - the
Control Panel, `DELETE /api/v1/settings/ini/<name>`, or `hmailserver
--set-setting <name>=` - and not in the file, which the new version puts back.
Left set, they keep the new version to the GitLab signer alone, with all of
that signer's checks: they restate its own values (see below). They also keep
it to the project's present path: once the project has moved to the group
`hmailserver` (below), a server that still has them refuses every release
signed after the move. Clear them before then.

Never point these three at the group's path, `hmailserver/hmailserver`. A
server from 6.2.28 to 6.3.3 checks no repository id at all, so pinned there it
would install whatever anyone who ever holds that path signs for a `v*` tag -
the id is what tells the project apart there, and only a server built after
6.3.3 checks it. Once the project has moved, a server still on 6.2.28 to 6.3.3
is upgraded by hand, once, to a release that knows the group's path.

What a server built after 6.3.3 checks
--------------------------------------

The trust is a list of this project's signers, compiled in: the GitHub
workflow, and the GitLab pipeline under two paths, the project's now and the
group's it is to move to. A certificate is accepted only when it is one
entry's whole: its identity, and that same entry's issuer, repository and
repository id.

| | GitHub | GitLab | GitLab, after the move |
|---|---|---|---|
| Signed | Every release from 6.2.19 to 6.3.3 | The releases from 6.3.5 until the project moves, by the `release-sign` job; 6.3.4 carries no bundle | The releases after the project moves to the group `hmailserver`; none yet |
| Identity (the certificate's subject alternative name begins with) | `https://github.com/Progressiverobot/hmailserver/.github/workflows/sign-release.yml@` | `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v` | `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v` |
| Issuer | `https://token.actions.githubusercontent.com` | `https://gitlab.com` | `https://gitlab.com` |
| Repository | `https://github.com/Progressiverobot/hmailserver` | `https://gitlab.com/Progressiverobot/hmailserver` | `https://gitlab.com/hmailserver/hmailserver` |
| Repository id (Fulcio's 1.3.6.1.4.1.57264.1.15) | `1267388815`, required | `86730559`, checked when the certificate carries one | `86730559`, required |
| Issued | Before 2026-09-18T00:00:00Z | Any time | Any time |
| Release | Only a release up to 6.3.3, the last it signed; where the workflow ran for a `v*` tag, that tag | The tag the certificate names | The tag the certificate names |

The rows after the repository are there because the names are not the
project's to keep, and because the feed is not the project's word either:

- **The GitHub signer is held to the past.** Nothing can be signed there by
  this project any more, and if the name were ever given to someone else, they
  could make a certificate that matches it. So a certificate issued from the
  suspension on is refused, by its notBefore, which Fulcio signs. The 80
  bundles on the re-hosted releases were all issued by 2026-09-15T01:00:18Z,
  and all record the id `1267388815`.
- **The GitLab signer is held to the project, not the path.** The project
  lives in a personal namespace, and a path can pass to someone else after a
  rename. Fulcio records the project's id beside the path, and the id is the
  project's for ever. No GitLab-issued certificate has been seen here yet, so
  the id is checked when the certificate carries one, rather than required: a
  server that refused every certificate for lacking an extension Fulcio turned
  out not to write could install nothing.
- **The path the project is to move to is trusted already, and only with the
  project's id.** The project is to leave the personal namespace for the group
  `hmailserver` (https://gitlab.com/hmailserver), which the owner reserved for
  it on 26 September 2026. A transfer changes the path in every certificate
  the pipeline is given and keeps the project's id, and a server trusts only
  what is compiled into it, so a server that knew only the present path would
  refuse every release signed after the move. Every server from 6.3.4 on knows
  both paths. For the group's the id is required: the path is not the
  project's until the move, and whoever holds it then - were the group lost,
  or the project moved on again - could sign for a `v*` tag of their own
  there, but never with this project's id. No setting relaxes it (see the
  three settings below). The cost is the project's: the
  project does not move until a release signed from the present path has shown
  that Fulcio records the id (its notes say, from `release-verify`), since if
  Fulcio wrote none, nothing signed from the group would install.
- **A file is held to its release.** A GitLab job signs for the tag it ran
  for, and the identity names that tag. A server built after 6.3.3 refuses a
  file whose certificate names another tag than the release the feed offers it
  as ("The certificate was made for the tag v6.3.5, not for v6.3.4, the release
  being installed."), when it downloads, when it applies and when it fetches its
  rollback image. So a feed that was tampered with cannot pass one GitLab-signed
  release off as another.
- **A GitHub-signed file installs only as a release up to 6.3.3.** 79 of the 80
  GitHub bundles were made by the workflow run from a branch, so their identity
  names no release, and the cut-off stops new certificates, not old files.
  Without a limit, a feed that was tampered with could offer the genuine 6.2.19
  installer as "6.3.9" to a server on 6.3.4, every check would pass, and the
  server would install a release too old for its own database. The workflow
  signed no release after 6.3.3, so nothing it signed is installed as a later
  one ("The certificate names ..., which signed no release after 6.3.3; this
  file is offered as 6.3.9."). A server on 6.3.3 still rolls back to its own
  GitHub-signed release.
- **On Windows, the installer says which release it is.** Every installer since
  6.2.19 carries a version resource, the fixed file version Inno Setup writes
  from `VersionInfoVersion` (`6.3.3.0` for 6.3.3). After the download, again at
  the apply, and for the rollback image, the server reads it and requires it to
  be the release it installs ("The installer says it is version 6.2.19.0, not
  6.3.9, the release it was offered as."); a file without one is refused. The
  Sigstore bundle binds those bytes to the signer, so the resource is the
  signer's own statement, whether or not `UpdateRequireAuthenticode` is on;
  where it is on, the Authenticode signature covers the resource as well. On
  Linux the update helper reads the package's own version and installs nothing
  that is not an upgrade. The resource names the version and not the stage: a
  `6.3.5-rc1` installer says `6.3.5.0`, as the final 6.3.5 will. What tells a
  pre-release from its final release is the tag the certificate names, which the
  release check holds for this project's signers. With `UpdateSigningIdentity`
  set that check is off, and then nothing on the server stops a site's own
  pre-release file offered as its final release: that site's signing is what
  keeps the two apart.
- **A download a check overtook is not kept.** A check that answers while a
  download runs can change the release on offer; the finished download is then
  not kept as the one waiting, and an apply refuses an installer held to no
  release, or to one not newer than the server's own. Nothing is downloaded
  while an update is being installed.
- **A version with a suffix is a pre-release.** A suffix is anything in the
  version but digits and dots - `6.3.5-rc1`, and `6.3.5rc1` too. The stable
  channel passes over such a version whatever the feed's `prerelease` flag says,
  so a genuine pre-release, signed for its own tag, does not reach a stable
  server because a feed called it stable.
- **On Windows, nobody else can change a file between its check and its run.**
  The server keeps the installer, the rollback image and the helper in the
  `Updates` folder of the data directory, and that folder and its `rollback`
  folder admit changes from SYSTEM and the Administrators, with full control,
  and - where the service does not run as SYSTEM - from the service's own
  account, `NT SERVICE\hMailServer`, with Modify, and from nobody else. The
  server makes them so when it makes them, and before every download and every
  apply it holds both open and checks them through that handle: while it holds
  them, neither they nor a folder above them can be renamed or swapped for a
  link. A folder an earlier version made, which took its parent's permissions,
  is given these and then emptied - a file planted while others could write there
  keeps its own owner and permissions, so it is deleted rather than kept - and
  the application log says what the folder allowed before; the waiting download
  goes with it and is fetched again. A folder that is a link, that is owned by
  anyone but SYSTEM, the Administrators or the service, whose permissions the
  server may not set, or with something in it that cannot be deleted, is refused
  with `HM6894`, and nothing is downloaded or applied; delete it, and the server
  makes it again. The apply then reads the SHA-256 of the installer and of the
  rollback image while it checks them, holding each open so that the signature,
  the version resource and the digest are all about the same bytes, and gives
  both digests to the helper. The helper opens each file so that nothing can
  write, rename or delete it, requires that digest, runs the program by the path
  Windows resolved that handle to, and lets go only when it has finished; a file
  with another digest is not run, and the outcome says so. The server holds its
  copy of the helper the same way, compared with the one in `Bin`, until it has
  started it, and refuses to start it where Windows cannot say where that copy
  is. The helper loads nothing from the folder it runs from: every DLL comes
  from System32.

`UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository`
replace their part of every entry, and a site that sets all three is left with
one signer, its own. A setting that changes a signer's part drops what went with
it: a new repository, or `-`, drops the repository id; a new identity or issuer
drops the GitHub cut-off and its last release; a new identity drops the release
check, since a site's workflow names its refs its own way. One thing no setting
drops: a certificate that names the group's path
(`https://gitlab.com/hmailserver/hmailserver//`, in any case) or its
repository is refused without the project's id `86730559`, whatever the three
make of the entries - with `UpdateSourceRepository=-`, and with an identity of
the site's own that names that path, too. A setting that restates a signer's
own value changes nothing about that signer. And where the settings
turn one signer into another's identity, issuer and repository without its
checks - the three GitLab values above, still set after an upgrade, do that to
the GitHub entry and to the group's - the copy is dropped and the unchanged
signer kept, so such a server trusts the project's present path alone.
`hmailserver --verify-update`, which the Linux update helper runs as root,
checks everything but the release, which it is not given; the helper holds the
package to an upgrade itself.

What the signing job produces
-----------------------------

The signing is the `release-sign` job of `.gitlab-ci.yml`, with
`build/ci/release-sign.sh`, written on 26 September 2026 for 6.3.5 onwards
([ContinuousIntegration.md](ContinuousIntegration.md), "The release"). It has not
run yet: 6.3.4 was released by hand, with an Authenticode signature and no bundle.
Every server built after 6.3.3 refuses a bundle that is not exactly what this
verifier reads, so the job is held to all of this. `build/check-release-chain.py`
fails on a change to the job that breaks a point it can see in the file, and the
first release is to be checked against the list by hand before it is published:

- [ ] **In `.gitlab-ci.yml` itself**, in the top-level pipeline of a protected
      `v*` tag: not a child or downstream pipeline, whose ID token carries no
      `ci_config_ref_uri` and gets no certificate, and not a branch or a
      schedule, whose identity ends in `@refs/heads/...`. The project's CI file
      stays `.gitlab-ci.yml`, since the file's path is in the identity.
- [ ] **The tag is the release**: tag `v6.3.4` is the release whose `tag_name`
      is `v6.3.4`, and every file the job signs is that release's. The
      certificate's identity then ends in `@refs/tags/v6.3.4`, which a server
      compares with the version it installs. A pre-release's tag carries a
      suffix (`v6.3.5-rc1`); the name alone does not make one.
- [ ] **An ID token for Sigstore**: `id_tokens: {SIGSTORE_ID_TOKEN: {aud:
      sigstore}}` on the job, from gitlab.com, on an image pinned by digest.
- [ ] **Rekor v1**: the public instance, `https://rekor.sigstore.dev`, whose
      key is compiled in; a `hashedrekord` entry, version 0.0.1, with the
      signature and the certificate in its body; an `integratedTime`; a signed
      entry timestamp or an inclusion proof to a signed checkpoint. cosign 3
      follows Sigstore's signing configuration, which can send a signature to
      Rekor v2 instead, with another key, another entry format and no
      `integratedTime`: every server would refuse that. Pin it:
      `cosign sign-blob --use-signing-config=false --rekor-url
      https://rekor.sigstore.dev --new-bundle-format --bundle <file>.cosign.bundle <file>`,
      or the cosign 2.x that `sign-release.yml` used.
- [ ] **A blob bundle**: a Sigstore bundle with `messageSignature` (a SHA-256
      digest and a signature) and the signing `certificate`, not a DSSE
      envelope or an attestation.
- [ ] **Names and places**: each artefact's bundle is named
      `<artefact>.cosign.bundle` and uploaded beside it to the generic package
      `hmailserver/<version>` (the tag without its `v`), with each release link
      named after the file it points to. Upload the files before the release's
      links are made: the feed leaves out a release whose links point at files
      the registry does not have, and refuses to publish at all when that
      release is the newest stable one.
- [ ] **Checked with this verifier**: every bundle verified before the release
      is published with `hmailserver --verify-update <file>` (Linux) from the
      same commit, not only with cosign. Check the certificate's identity,
      issuer, repository and id against the table above, and put them in the
      release's notes.
- [ ] **The project's path, and its id**: the job signs as the path it runs
      at, `Progressiverobot/hmailserver` until the project moves to the group
      and `hmailserver/hmailserver` after, and from no other. Before the move,
      the notes of a release signed from the present path must show the id
      `86730559` in the certificate: a server requires it for the group's path,
      so the project does not move until one has.

How the job meets each point: it is a job of the tag's own pipeline and never a
trigger; the tag is checked as an annotated tag signed by a key in
`.gitlab/allowed_signers`, and `Version.h` must say the version the tag names
before anything is signed; the job has `id_tokens: {SIGSTORE_ID_TOKEN: {aud:
sigstore}}` and runs cosign's own image from `gcr.io` pinned by digest
(`HM_COSIGN_IMAGE`); it signs with `--new-bundle-format --rekor-url
https://rekor.sigstore.dev`, and with `--use-signing-config=false` wherever the
cosign in the image has that option, then refuses a bundle that is not a
Sigstore bundle with a `messageSignature` and a hashedrekord 0.0.1 entry with an
`integratedTime` and the log's promise or proof; both refuse to run at a
project path (`CI_PROJECT_PATH`) other than the two above, and name the
identity after the one they run at; `release-verify` runs
`hmailserver --verify-update` from `linux-build` of the same commit on every
file, requires that identity, and writes the notes' verification section
with the repository id as the certificate states it; and `release-publish`
uploads every file to `hmailserver/<version>` before it creates the release with
a link per file named after it, then renders the feed from the API and requires
the new release in it with a Windows, a Debian and an RPM file and their
bundles (`build/ci/release-chain.py`).

What the site serves
--------------------

Every address a server builds from the feed is a file on the site, answered 200
with no redirect.

| Address | What it is | Who reads it |
|---|---|---|
| `/releases/latest` | The newest release that is not a pre-release, one release object | The stable channel's built-in address, since 6.3.4 |
| `/releases.json` | Every release, newest first, a list | The pre-release channel's built-in address, and the value of `UpdateFeedUrl` above |
| `/releases/tags/v<version>` | One release | The rollback's lookup, for either of the two above |
| `/releases/` | A page showing the same list | A browser, or a client that followed the redirect |
| `/` | A page saying what the site is | A person |

GitLab Pages serves a file at its path. It answers a directory's path with a
redirect to the path with a slash, and the path with the slash with the
directory's `index.html`. It ignores the query string. That is why `/releases`
and `/releases?per_page=10` are redirects: `releases/` is a directory, because
`releases/latest` is in it.

A release is in the shape of GitHub's Releases API, which is what 6.2.28 and
every later version read, and it has only the fields the server reads:
`tag_name`, `name`, `html_url` (the release's page on GitLab), `published_at`
(UTC), `draft` (always false) and `prerelease`. Each file of the release is
under `assets`, with its `name`, its `browser_download_url` in the project's
package registry, and the `size` and `sha256:` `digest` the registry records. A
download is held to both. Each installer's Sigstore bundle is an asset of its
own, named after its file with `.cosign.bundle` added, which is how the server
finds it. The JSON escapes `<`, `>` and `&`, and the page at `/releases/`
escapes the list again, so nothing in a release's name can run on the domain.

GitLab has no pre-release flag. A release is a pre-release when its tag has a
suffix, anything after `v` but digits and dots (`v6.2.23-alpha1`, `v6.3.5rc1`),
which is how every pre-release so far was published,
and the one mark a server reads whatever the feed says. A release named as a
pre-release whose tag has no suffix breaks that rule and is treated like a
broken link below. `releases/latest` is the highest stable
version, not the most recently dated one. A tag that is not `v` and a digit
(`build-inputs-1`) is not a server release and is left out. So is a release
GitLab calls upcoming, whose date has not come. A release with a link that
breaks the convention every release so far follows - the link named after its
file, the file in the package `hmailserver/<the tag without its v>` - is left
out with a warning, unless it is the newest stable release, which stops the
render instead, so that the site keeps what it had.

GitLab Pages gives `releases.json` the JSON type and guesses the type of the
files without an extension from their content, which makes them text. The
server does not look at the type.

How it is published
-------------------

`build/generate-update-feed.py` reads the releases and the package registry
through GitLab's public API, with no token, and writes the site into `public/`.
The `pages` job in `.gitlab-ci.yml` runs it on GitLab's shared runners, and
GitLab Pages deploys what it writes. The job runs for a push to `master`, for a
`v*` tag, and for a pipeline schedule on `master`, which runs that job alone.
The schedule picks up a release made or edited after its tag's pipeline ran.
What the job publishes depends on the releases at that moment and not on the
commit, apart from the script itself. A merge request never runs it.
[ContinuousIntegration.md](ContinuousIntegration.md) says how the schedule is
made. The `checkers` job runs the script's self-test in every other pipeline,
so a change that breaks a compiled feed address fails before it lands.

The script can also be run by hand, from the repository root:

```sh
python3 build/generate-update-feed.py                   # write public/
python3 build/generate-update-feed.py --serve-check public
python3 build/generate-update-feed.py --check --site https://updates.progressiverobot.com
python3 build/generate-update-feed.py --self-test
```

`--serve-check` serves a tree as GitLab Pages would and reads every address a
server builds, on both channels and for the rollback of every release, with a
copy of the server's parser. It fails when a built-in address gives its
channel no release, and when the stable release has no Windows installer with
its bundle. `--check --site` fails when the deployed site is not what the
releases say now, which is how to tell a stale feed from a current one.
`--self-test` needs no network.

The name is a CNAME, `updates` to `progressiverobot.gitlab.io`, served DNS-only
(not proxied) at Cloudflare. The domain is added and verified in the project's
Pages settings, and GitLab issues and renews its Let's Encrypt certificate.

A server reads the site anonymously, so Pages must be visible to everyone
(Settings, General, Visibility, Pages). If Pages is restricted to the project's
members, GitLab answers an anonymous request with a redirect to its sign-in page
at `projects.gitlab.io/auth`. A server reports that as "The update feed answered
HTTP 302.", and `--check --site` reports every file as a 302.

Settings
--------

This adds no setting. `UpdateFeedUrl`, `UpdateAutoDownload` and the three trust
settings above are the `[Settings]` keys the live update has read since 6.2.28,
and nothing about them moves here. Since 6.3.4, all
`[Settings]` values live in the database, and this changes none of them.
