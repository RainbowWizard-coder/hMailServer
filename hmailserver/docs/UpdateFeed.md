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

*The site has been deployed since 26 September 2026 and answers over HTTPS with a
certificate for its name. 6.3.5, published on 29 September 2026, is the first
release this project's GitLab pipeline signed; 6.3.4 was built and signed by hand,
as below. A server on 6.3.4 installs 6.3.5 by its live update: see
[From 6.3.4 on, by the live update](#from-634-on-by-the-live-update).*

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
  `gitlab.com/hmailserver/hmailserver/-/releases/`. `GET /api/v1/update`
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
- **A release signed on GitLab does not install through an old server's live
  update.** From 6.3.5 the releases are signed by this project's GitLab CI
  pipeline for their `v*` tag, since nothing can be signed on GitHub any more.
  6.3.4 itself was built and
  Authenticode-signed by hand and carries no Sigstore bundle, so no server
  installs it by the live update: one offered it says it cannot be verified and
  downloads nothing (`UpdateDownloader.cpp`; the feed's check lets it through
  by name, `UNSIGNED_BY_DESIGN` in `build/generate-update-feed.py`). It is
  installed by hand, as below. An old server's check finds a release signed
  on GitLab - 6.3.5 - but its download is refused: "The certificate names
  `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`,
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
signer at the path the project began at, they let the old server's live update
install 6.3.5, the one release signed there, while 6.3.5 is the newest stable
release; from 6.3.6 the releases are signed at the group's path, which these
settings must not name (below):

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
values are what the new version compiles in, and 6.3.5's certificate names exactly
them (its release notes, "Verification"); that a server from 6.2.28 to 6.3.3 so
configured installs a GitLab-signed release has not been tried.

Leave the lines in place until the new version is running: the old server
verifies the installer once more as it hands it over, with the same settings.
Then clear all three. On 6.3.4 and 6.3.5 that is done the way the last bullet
above clears `UpdateFeedUrl` - the Control Panel, `DELETE
/api/v1/settings/ini/<name>`, or `hmailserver --set-setting <name>=` - and not
in the file, which those two versions put back. From 6.3.6 the three are read
from the file alone ([Where the trust is read from](#where-the-trust-is-read-from)):
delete the lines from `[Settings]` with an editor run as an administrator, or on
Linux with `sudo hmailserver --set-setting <name>=`, and restart the service.
Left set, they keep the new version to the GitLab signer alone, with all of
that signer's checks: they restate its own values (see below). They also keep
it to the path the project began at: the project moved to the group
`hmailserver` on 29 September 2026 (below), and a server that still has them
refuses every release signed after the move, from 6.3.6. Clear them.

Never point these three at the group's path, `hmailserver/hmailserver`. A
server from 6.2.28 to 6.3.3 checks no repository id at all, so pinned there it
would install whatever anyone who ever holds that path signs for a `v*` tag -
the id is what tells the project apart there, and only a server built after
6.3.3 checks it. Once a release newer than 6.3.5 is published, a server still on
6.2.28 to 6.3.3 is upgraded by hand, once, to a release that knows the group's
path.

From 6.3.4 on, by the live update
---------------------------------

A server on 6.3.4 knows the GitLab signer and reads this feed without being told,
and 6.3.5 is the first release it can install by its live update. The maintainers
tried that on 29 September 2026 on a Windows Server 2025 machine reverted to a
clean snapshot for each run, against the published 6.3.5 and this feed:

- **A service running as LocalSystem** - an upgrade from an earlier version keeps
  that account ([ServiceAccount.md](ServiceAccount.md)), and a new installation
  takes it with `/serviceaccount=localsystem` - found 6.3.5, downloaded it,
  verified it as signed by the pipeline for `v6.3.5`, took its backup and
  installed it through the updater, unattended, in its window. It came back as
  6.3.5 with the schema upgraded from 6104 to 6105 after about 24 seconds
  without mail, and its domains, accounts, messages and settings as they were.
- **Without a rollback image.** 6.3.4 carries no Sigstore bundle, so a server on
  6.3.4 cannot verify its own version's installer to keep as the image to go
  back to, and installs 6.3.5 without one. The log says so before the install
  ("Update: no rollback image for hMailServer 6.3.4 could be had (release 6.3.4
  carries no x64 installer with a Sigstore bundle...)"): a 6.3.5 that did not
  start would need a manual reinstall. Every
  release from 6.3.5 on carries its bundle, so a server on 6.3.5 has an image
  for the next one.
- **A service running as `NT SERVICE\hMailServer`**, the account a new 6.3.4
  installation gives it, found, downloaded and verified 6.3.5, then logged
  `HM7463` once and left the install to an administrator
  ([ServiceAccount.md](ServiceAccount.md)): download the installer from the
  release page, check that `Get-AuthenticodeSignature` reports it Valid, and run
  it as an administrator. Nothing was left half-installed. That is still how such
  a server on 6.3.4 or 6.3.5 reaches 6.3.6, once, since the refusal is compiled
  into them; from 6.3.6 an administrator installs each release with one click,
  *Install update* in the Control Panel on the server, which asks Windows once
  and checks the installer itself (below, and
  [ServiceAccount.md](ServiceAccount.md), "The live update").
- **On Linux, from the `.deb` and the `.rpm`.** Tried on 29 September 2026 in
  two throwaway systemd containers, each a 6.3.4 server installed from the
  release's package with PostgreSQL 18 beside it, against the published 6.3.5
  and this feed. On Ubuntu 26.04 a server with `UpdateCheckEnabled=1`,
  `UpdateAutoDownload=1` and an `UpdateWindow` found 6.3.5 by
  `hmailserver_6.3.5_amd64.deb`, downloaded it, verified it as signed by the
  pipeline for `v6.3.5`, took its backup and handed it to `hmailserver-update`
  in its window, unattended. The helper verified it again as root, stopped the
  unit, installed it with `apt-get`, whose post-install step upgraded the schema
  from 6104 to 6105, started the unit and wrote `ok 6.3.5`. The server was
  without listeners for about six seconds, and its mail was as it was. On
  Fedora 44 the same happened by hand over the REST API (`POST
  /api/v1/update/check`, `.../download`, `.../install`) with
  `hmailserver-6.3.5-1.x86_64.rpm` and `dnf`. Neither had a rollback image, as
  above.
- **On Linux up to 6.3.5, the backup before an unattended apply needs a
  destination outside the data directory.** The packaged unit lets the service
  write only under `/var/lib/hmailserver` and `/var/log/hmailserver`. A
  destination under `/var/lib/hmailserver` is inside the data directory, and on
  6.3.4 and 6.3.5 a backup there copies itself into itself and fails (`HM5014`,
  "Could not backup data directory"), every time. With
  `UpdateBackupBeforeApply=1`, the default, the update is then not applied, a
  quarter hour after a quarter hour: the Ubuntu server's was refused for an hour
  and a half. Make the destination elsewhere, give it to the service, and add it
  to the unit:

  ```sh
  sudo install -d -o hmailserver -g hmailserver -m 0750 /var/backups/hmailserver
  sudo systemctl edit hmailserver      # [Service]  ReadWritePaths=/var/backups/hmailserver
  sudo systemctl restart hmailserver
  ```

  then set the destination to `/var/backups/hmailserver` (the Deck's Backup
  page, or `PUT /api/v1/settings/backup`). From 6.3.6 a destination inside the
  data directory works (below), but a backup there is on the disk it backs up,
  so one elsewhere is still the better place.

The test found three things in the live update of 6.3.4 and 6.3.5, which 6.3.6
changes:

- **The apply's outcome was forgotten a second after the start.** The service
  reads the helper's outcome at start-up, and the check that follows replaced
  the whole update status: `Status.UpdateApplyOutcome`, `apply` in `GET
  /api/v1/update` and the Control Panel said nothing of the update, and only
  the log had it. From 6.3.6 a check keeps it; it stays until the service stops
  or the next apply's outcome replaces it. On Linux the same: with the scheduled
  check on, `apply` in `GET /api/v1/update` was empty at every look after the
  update, because the check ran a second after the outcome was read.
- **A first check could fail with "The response carried no HTTP headers."** On
  the new machine the first check failed with it after 10.8 seconds, and the next,
  half a minute later, took 1.2 seconds and succeeded. The sentence meant only
  that no answer had arrived; the likeliest cause, a first TLS handshake slowed
  by Windows checking a certificate chain for the first time until the site gave
  up on the request, is a guess it could not confirm. From 6.3.6 the sentence
  says who ended the connection, after how many bytes and seconds, the TLS
  handshake's share of them, and how the read ended ("`<host>` ended the
  connection without answering, `<n>` s after it was made, `<m>` s of them in the
  TLS handshake (`<how the read ended>`)"), and a check that failed is due again
  once, ten minutes later, before it waits `UpdateCheckHours` again: the
  scheduled task, which runs every quarter hour, reads the feed at its first run
  from then, a quarter of an hour after a scheduled check that failed
  (`nextCheck` in `GET /api/v1/update` says when it is due).
- **A service that cannot install downloaded the installer at every start.**
  With `UpdateAutoDownload=1`, a service running as `NT SERVICE\hMailServer`
  fetched the whole installer, 88 MB for 6.3.5, each time it started, and left
  it to an administrator each time. From 6.3.6 the verified copy is kept across
  a restart and verified again where it is (see [What a server built after 6.3.3
  checks](#what-a-server-built-after-633-checks)).

The Linux test found three more, which 6.3.6 changes as well:

- **A backup destination inside the data directory failed every backup**, as
  above: the copy of the data directory walked into its own staging folder, left
  a nested tree behind, and every unattended apply was refused. From 6.3.6 the
  copy leaves out the destination, the temp directory and the backup's staging
  folder wherever they are inside the data directory - recognised by what they
  are, not by how their path is spelt, so `/var/lib/hmailserver2` is not taken
  for `/var/lib/hmailserver` and case and separators on Windows do not matter -
  and the backup log names each one. A restore empties the data directory around
  them and around the archive it is restoring from. See
  [Backup.md](Backup.md), "A destination inside the data directory".
- **Every backup carried the `Updates` folder**, so the backup taken before an
  apply held the package the apply was about to install: 15 MB of the Ubuntu
  server's 15.4 MB archive, beside 353 bytes of mail; on Windows it is the
  installer and the rollback image. From 6.3.6 a backup leaves the folder out, as
  it leaves out the recoverable store, and a restore leaves this machine's own
  folder where it is - so a restored server never reads an apply's outcome it did
  not make, and an older archive's copy is not put back.
- **Root's check and the server read the trust from two places.** On a Linux
  package `hmailserver --verify-update`, the helper's check as root, reads the
  five trust settings from `/etc/hmailserver/hMailServer.ini` alone, while the
  server read the values stored in the database, where the REST API and the Deck
  write. On the Fedora server an `UpdateSigningIdentity` set over REST was used by
  the server and not by root, and the same value set with `hmailserver
  --set-setting`, which writes the file, made root refuse the release: a site's
  own signer set over REST would have had every apply fail as root, and a value
  cleared over REST but still in the file would have kept root on the old trust.
  From 6.3.6 the server reads them from the file too, on every platform, and
  nothing that runs as the service may change them ([Where the trust is read
  from](#where-the-trust-is-read-from)).

Up to 6.3.5 these are the servers' own, and an upgrade to 6.3.6 is what changes
them.

What a server built after 6.3.3 checks
--------------------------------------

The trust is a list of this project's signers, compiled in: the GitHub
workflow, and the GitLab pipeline under two paths, the one the project began at
and the group's it moved to on 29 September 2026. A certificate is accepted only
when it is one
entry's whole: its identity, and that same entry's issuer, repository and
repository id.

| | GitHub | GitLab | GitLab, after the move |
|---|---|---|---|
| Signed | Every release from 6.2.19 to 6.3.3 | 6.3.5, by the `release-sign` job, the one release signed before the project moved; 6.3.4 carries no bundle | Every release from 6.3.6, since the project moved to the group `hmailserver` on 29 September 2026 |
| Identity (the certificate's subject alternative name begins with) | `https://github.com/Progressiverobot/hmailserver/.github/workflows/sign-release.yml@` | `https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v` | `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v` |
| Issuer | `https://token.actions.githubusercontent.com` | `https://gitlab.com` | `https://gitlab.com` |
| Repository | `https://github.com/Progressiverobot/hmailserver` | `https://gitlab.com/Progressiverobot/hmailserver` | `https://gitlab.com/hmailserver/hmailserver` |
| Repository id (Fulcio's 1.3.6.1.4.1.57264.1.15) | `1267388815`, required | `86730559`, required from 6.3.6 (6.3.4 and 6.3.5 checked it when the certificate carried one) | `86730559`, required |
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
  lived in a personal namespace until 29 September 2026, and a path can pass to
  someone else after a rename or a move. Fulcio records the project's id beside
  the path, and the id is the project's for ever. Servers 6.3.4 and 6.3.5 were
  written before any GitLab-issued certificate had been seen, so they check the
  id at that path when the certificate carries one rather than require it: a
  server that refused every certificate for lacking an extension Fulcio turned
  out not to write could install nothing. 6.3.5's, the first and the only one
  signed there, records it (`86730559`), so from 6.3.6 the id is required at
  that path as at the group's, and a certificate for it without the id, or with
  another project's, is refused - what a project that is not this one, made at
  the path the project has left, would present.
- **The path the project moved to was trusted before the move, and only with
  the project's id.** The project left the personal namespace on 29 September
  2026 for the group `hmailserver` (https://gitlab.com/hmailserver), which the
  owner had reserved for it on 26 September 2026. A transfer changes the path
  in every certificate the pipeline is given and keeps the project's id, and a
  server trusts only what is compiled into it, so a server that knew only the
  first path would refuse every release signed after the move. Every server
  from 6.3.4 on knows both paths. For the group's the id is required: the path
  was not the project's until the move, and whoever holds it later - were the
  group lost, or the project moved on again - could sign for a `v*` tag of
  their own there, but never with this project's id. No setting relaxes it (see
  the three settings below). The cost was the project's: it did not move until
  a release signed from the first path had shown that Fulcio records the id
  (its notes say, from `release-verify`), since had Fulcio written none,
  nothing signed from the group would install. 6.3.5 showed it (`86730559`),
  and the project moved the same day.
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
- **A verified installer outlives a restart, and is verified again.** From
  6.3.6, a download that finds the release's installer and its bundle already in
  the `Updates` folder holds them to everything a fresh download is held to - the
  feed's size and digest, the bundle, the version resource, Authenticode where it
  is required - and fetches nothing when they pass ("Update: hMailServer 6.3.6 is
  already in ... and verified again"); a copy that no longer passes is deleted,
  bundle and all, and fetched again. Until a check has said what is on offer -
  after a start whose first check failed - the task's sweep of the folder leaves
  it alone, so the copy is still there for the next check that succeeds. Up to
  6.3.5 the verdict lived in the process alone, so every restart fetched the
  installer again, and a service
  that cannot install it (below) with `UpdateAutoDownload=1` downloaded the whole
  installer each time it started.
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
drops: a certificate that names one of the project's two GitLab paths - the
group's (`https://gitlab.com/hmailserver/hmailserver//`) or, from 6.3.6, the one
it began at (`https://gitlab.com/Progressiverobot/hmailserver//`), in any case -
or its repository is refused without the project's id `86730559`, whatever the
three make of the entries - with `UpdateSourceRepository=-`, and with an identity
of the site's own that names one of those paths, too. A setting that restates a signer's
own value changes nothing about that signer. And where the settings
turn one signer into another's identity, issuer and repository without its
checks - the three GitLab values above, still set after an upgrade, do that to
the GitHub entry and to the group's - the copy is dropped and the unchanged
signer kept, so such a server trusts the path the project began at alone.
`hmailserver --verify-update`, which the Linux update helper runs as root,
checks everything but the release, which it is not given; the helper holds the
package to an upgrade itself.

### Where the trust is read from

The five settings that make the trust - `UpdateSigningIdentity`,
`UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and
`UpdateLogPublicKeyFile` - are read from the `[Settings]` section of
`hMailServer.ini` alone from 6.3.6, on every platform, and never from the
database. They are the one deliberate exception, beside `PasswordPepper`, to
the rule that a setting lives in the database, and the reason is security: the
process an update is checked for must not choose what it is checked against.
On Linux the service's database user may write the settings store, and the
REST API and the Deck write there; root's check exists so that a service that
has been taken over cannot have root install something the release workflow
never signed, and `hmailserver --verify-update` opens no database - so the
server reads the five with the same parse from the same file, and the two checks
are one check.

- **Where they are set.** On Linux, as root: `sudo hmailserver --set-setting
  UpdateSigningIdentity=...` (`NAME=` with nothing after it clears one), then
  restart or reload the service; root's check uses the new value at once. On
  Windows, in the file itself with an editor run as an administrator, then
  restart the service. `[SettingsOverride]` does not apply to them: a line for
  one there is ignored and named in the error log (`HM5809`) at every start.
- **What refuses them.** COM (`Settings.SetIniSetting` and `DeleteIniSetting`),
  the REST API (`PUT` and `DELETE /api/v1/settings/ini/<name>`, 409), and so the
  Control Panel, the Deck, a settings profile and the audit trail's put-back,
  each with the sentence that names the file and says how the setting is set.
  A write of the value the file already holds, and the removal of one the file
  does not set, change nothing and are taken, so that a configuration read and
  written back whole is not turned away. The Control Panel shows the five and
  does not edit them.
- **A value an earlier version stored.** 6.3.4 and 6.3.5 kept the five in the
  database like any other setting. At 6.3.6's first start such a row is taken
  out; where it held something the file does not, the error log says so once,
  as `HM5809`, naming the setting and both values, so that an administrator who
  set a signer over REST learns it no longer applies and can put it in the file.
  It is never written into the file: that would be the service choosing the
  trust after all. Read both values: where the file has a line of its own, it is
  the file's that applies from then on, even where the database's was the
  narrower of the two - a repository check the file switches off with `-` while
  the database kept it on, or an older path left in the file after the database's
  was cleared. Up to 6.3.5 on Linux root's check read the file's already, and an
  apply had to pass both; from 6.3.6 the file's is the only one, so make it the
  one wanted.
- **More than one server on one database.** The first 6.3.6 server to start
  takes the rows out for all of them, so a 6.3.5 server still running on the
  same database reads the five from its own file, or the built-in trust, from its
  next start or reinitialise. Put a signer of the site's own into every server's
  file before the first of them is upgraded.
- **Backups.** An archive does not carry them, and a restore does not put back
  one an older archive carried; the application log names it. Copy them to a
  standby or a new machine with the file, as `PasswordPepper` is copied
  ([WarmStandby.md](WarmStandby.md)).
- **On Windows** there is no second check to agree with: `hMailServer.Updater.exe`
  runs as the service's own account, and the service may apply an update only
  when that account is an administrator (LocalSystem), so the helper holds the
  installer to the SHA-256 the service verified rather than verifying it again.
  The service's check is the only one, and it reads the trust from the same
  file as on Linux. Under `NT SERVICE\hMailServer`, which a new installation
  runs as, the file is read-only to the service, so there too the service cannot
  choose its trust. And there the service's check is not the only one: from
  6.3.6 the administrator's install checks the installer again, elevated, on
  its own copy - the Sigstore bundle against the trust it reads from the same
  file, in a child process with the administrator's rights taken away, and the
  Authenticode signature with the publisher pinned, the version and the
  architecture ([An administrator's install](#an-administrators-install)).

An administrator's install
--------------------------

A Windows service running as `NT SERVICE\hMailServer` - a new installation's
account since 6.3.4 - is not an administrator and cannot run an installer. It
checks for the update, downloads it and verifies it as any server does, and
from 6.3.6 an administrator installs it with *Install update* on the Status page
of the Control Panel on the server. Windows asks for permission once; the
Control Panel then starts `hMailServer.exe /InstallUpdate` elevated, which copies
the installer and its bundle out of the `Updates` folder into
`AdminUpdate\<id>`, a folder only administrators can change, and checks the copy
itself, trusting nothing the service said about it:

- the Sigstore bundle, as the service checked it, with the trust read from
  `hMailServer.ini` - in a child process whose Administrators group, and every
  other group UAC takes away, is deny-only, at low integrity and in a job, which
  is given the bundle and the installer's SHA-256 through a pipe;
- the Authenticode signature, with the publisher pinned to Progressive Robot
  Ltd and the Microsoft chain its certificate is issued under, and its
  revocation checked online - or, where the installed `hMailServer.exe` is
  signed by another publisher, held to that one and to the root its
  certificate chains to;
- the version in the installer's version resource, which must be the release
  offered and newer than the running server, and the architecture in its
  description.

The service takes the backup and fetches the rollback image, which the stage
checks the same way, held to the running version; then it drains, and
`hMailServer.Updater.exe` runs the installer as the administrator, rolls back if
the service does not come back, and opens the Control Panel again, signed in, on
the outcome. `UpdateState` is 6 while the service prepares and 7 when it is
ready; `GET /api/v1/update` says `installedBy: administrator` and
`POST /api/v1/update/install` answers 409, code
`update_install_by_administrator`, with the sentence that says where to click.
[ServiceAccount.md](ServiceAccount.md), "The live update", has each step, what
the service can and cannot influence, and the error codes.

What the signing job produces
-----------------------------

The signing is the `release-sign` job of `.gitlab-ci.yml`, with
`build/ci/release-sign.sh`, written on 26 September 2026 for 6.3.5 onwards
([ContinuousIntegration.md](ContinuousIntegration.md), "The release"). It first
ran for 6.3.5, on 29 September 2026; 6.3.4 was released by hand, with an
Authenticode signature and no bundle. Every server built after 6.3.3 refuses a
bundle that is not exactly what this verifier reads, so the job is held to all of
this. `build/check-release-chain.py` fails on a change to the job that breaks a
point it can see in the file, and a release is checked against the list by hand
before it is published, as 6.3.5 was:

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
- [ ] **The project's path, and its id**: the job signs as the group's path,
      `hmailserver/hmailserver`, where the project has been since 29 September
      2026, and from no other; 6.3.5, before the move, was signed as the first
      path, `Progressiverobot/hmailserver`. The release's notes must show the id
      `86730559` in the certificate: a server requires it for the group's path.

How the job meets each point: it is a job of the tag's own pipeline and never a
trigger; the tag is checked as an annotated tag on a commit `master` has,
signed by the one key `master`'s `.gitlab/allowed_signers` lists, and
`Version.h` must say the version the tag names
before anything is signed; the job has `id_tokens: {SIGSTORE_ID_TOKEN: {aud:
sigstore}}` and runs cosign's own image from `gcr.io` pinned by digest
(`HM_COSIGN_IMAGE`); it signs with `--new-bundle-format --rekor-url
https://rekor.sigstore.dev`, and with `--use-signing-config=false` wherever the
cosign in the image has that option, then refuses a bundle that is not a
Sigstore bundle with a `messageSignature` and a hashedrekord 0.0.1 entry with an
`integratedTime` and the log's promise or proof; both refuse to run at a
project path (`CI_PROJECT_PATH`) other than the group's, and name the identity
after it; `release-verify` runs
`hmailserver --verify-update` from `linux-build` of the same commit on every
file, requires that identity, and writes the notes' verification section
with the repository id as the certificate states it; and `release-publish`
uploads every file to `hmailserver/<version>` before it creates the release with
a link per file named after it, then renders the feed from the API and requires
the new release in it with a Windows, a Debian and an RPM file for x86-64 and
AArch64 and both AppImages, each with its bundle (`build/ci/release-chain.py`).
Before it signs anything, and again before it uploads, it renders the site with
the new release in it and refuses a feed a server would not read whole (below).

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
| `/releases.atom` | The same releases, newest first, as an Atom feed | A feed reader; no server reads it |
| `/` | A page saying what the site is, with the download links | A person |
| `/download/<name>` | A 302 to a file of the newest stable release | A person, a script, a page that links a download |
| `/download/` | A 302 to that release's page on GitLab | A person |

GitLab Pages serves a file at its path. It answers a directory's path with a
redirect to the path with a slash, and the path with the slash with the
directory's `index.html`. It ignores the query string. That is why `/releases`
and `/releases?per_page=10` are redirects: `releases/` is a directory, because
`releases/latest` is in it.

### The download links

`/download/<name>` always leads to the release `releases/latest` is: the newest
stable version. `<name>` is the file's name with its version taken out, the
same name each release's own permanent link has:

| Name | The file |
|---|---|
| `hMailServer-x64.exe` | The Windows installer |
| `hMailServer-arm64.exe` | The Windows installer for ARM64 |
| `hmailserver_amd64.deb`, `hmailserver_arm64.deb` | The Debian and Ubuntu packages |
| `hmailserver.x86_64.rpm`, `hmailserver.aarch64.rpm` | The RPM packages |
| `hMailServer-x86_64.AppImage`, `hMailServer-aarch64.AppImage` | The AppImages |

Each has its Sigstore bundle beside it at the same name with `.cosign.bundle`
added, for example `/download/hMailServer-x64.exe.cosign.bundle`. A name has a
link only when that release carries its file. A name it does not carry answers
404, never a file of another release. 6.3.5 carries the x64 installer, the amd64
`.deb` and the x86-64 `.rpm`, each with its bundle (6.3.4 carried the same three
and no bundle), so its other names answer 404 until a stable release carries
them again. The release pipeline carries the
AArch64 `.deb` and `.rpm` and both AppImages from 6.3.6, each with its bundle, so
`hmailserver_arm64.deb`, `hmailserver.aarch64.rpm`, `hMailServer-x86_64.AppImage`
and `hMailServer-aarch64.AppImage` answer from the first stable release that has
them, and a server installed from any of them finds its file in the feed by the
name its update check asks for (`hmailserver_<v>_arm64.deb`,
`hmailserver-<v>-1.aarch64.rpm`, `hMailServer-<v>-<arch>.AppImage`). The ARM64
Windows installer's name answers 404 until a release carries it: 6.3.6 is the first,
as a preview that has never run on ARM64 hardware ([WindowsOnArm.md](WindowsOnArm.md)).
`--serve-check` fails a stable release that
carries any server's file without its bundle, unless it is unsigned by design.

From 6.3.6 a release also carries each distribution release's own package -
`hmailserver_<v>-1~<release>_<arch>.deb` for Debian 12 and 13 and Ubuntu 24.04,
`hmailserver-<v>-1.<release>.<arch>.rpm` for Fedora 44 and 43, EL 10 and 9 and
openSUSE Leap 16.0 and Tumbleweed - the source tarball, the Helm chart, a
`.sigstore.json` copy of each primary file's bundle and the SLSA provenance. None
of them has a `/download/` name: a server never asks for them by name, and the
release's own permanent links on GitLab name each one, their paths
(`direct_asset_path`) the file's name with the version taken out, as for the
files above: `hmailserver_debian12_amd64.deb`, `hmailserver.el9.x86_64.rpm`,
`hmailserver.tar.gz`, `hmailserver-chart.tgz`. They are what the apt and
dnf repositories serve once those are published ([PackageManagers.md](PackageManagers.md)).

**GitLab's own permanent links.** Every file of a release is also at
`https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/<name>`
while its release is the latest, and always at `.../-/releases/v<version>/downloads/<name>`,
`<name>` being its link's path: the file's name with the version taken out.
GitLab picks the latest by date, a pre-release included, where `/download/` follows
the newest stable version. From 6.3.6 the path of an SBOM, a bundle and a bundle's
`.sigstore.json` copy is its file's with the suffix added
(`hMailServer-x64.exe.cdx.json`, `hMailServer-x64.exe.cosign.bundle`), so the
installer and the bundle that verifies it are both at `permalink/latest` under the
same names as here, and no path holds a version or the `~` of a `.deb`'s, which
GitLab refuses in a path. 6.3.5's bundles keep the version in their paths
(`hMailServer-6.3.5-x64.exe.cosign.bundle`), and 6.3.4's SBOMs theirs. A link's
name is always the file's own, with the version; that is what a server and this
feed read ([ContinuousIntegration.md](ContinuousIntegration.md), "The links' names
and paths").

A server installed from one of those distribution releases' packages is, to its
update check, a Debian or an RPM installation like any other: dpkg or rpm owns
`hmailserver`. Its live update offers it the generic file of the next release,
built on Ubuntu 26.04 (the `.deb`, the x86-64 `.rpm`) or Fedora 44 (the aarch64
`.rpm`), and hands it to apt or dnf, which install it where its dependencies are
met and refuse it where they are not. 6.3.4's generic `.deb` asks for `libc6 (>=
2.38)`, `libldap2 (>= 2.6.2)`, `libssl3t64` and `libstdc++6 (>= 14)`: Debian 13 and
Ubuntu 24.04 have them, so there the live update replaces the release's own build
with the Ubuntu 26.04 one, which runs; Debian 12 has none of them, and apt refuses
it. dnf resolved the generic x86-64 `.rpm` on Fedora 43 and 44 and AlmaLinux 10 and
refused it on AlmaLinux 9 (PackageManagers.md). Nothing is installed when they
refuse; the server says why. Update such a server with its own release's file of
the next version, or through the apt and dnf repositories once they exist. Building those packages without the live
update, so that their update check names the package manager's command instead
(`-DHM_LIVE_UPDATE=OFF` and `HM_UPDATE_COMMAND`, [LiveUpdateKinds.md](LiveUpdateKinds.md)),
waits for the repositories: until they exist, that command would find nothing.

### How much the site holds

A release as 6.3.6 is has 154 links (Roadmap3, "Release collects every file"). The
site that carries it (`build/ci/release-chain.py`'s `check_budget`, which the
`release` job runs before anything is signed and `release-publish` before anything
is uploaded, both refusing past a limit, with the apt and dnf repositories kept as
`package-repos`' `--keep` keeps them):

| | 6.3.6 alone | Grows by, per release of its shape | The limit |
|---|---|---|---|
| `releases.json` | 52,334 bytes | 52,331 bytes | 4 MiB, the largest feed a server reads (`MAX_FEED_BYTES`, `UpdateChecker.cpp`): about eighty releases of this shape |
| `releases/latest` | 51,396 bytes | nothing: it is the one release | 4 MiB |
| `_redirects` | 33 rules in 5,700 bytes: the feed's 17 (the download page and eight files with their bundles) and the apt and dnf repositories' 16 | 16 rules, 2,942 bytes | 1,000 rules and 64 KiB, which Pages reads: twenty-one releases of this shape fit, and the twenty-second needs `--keep` ([PackageManagers.md](PackageManagers.md), "What the repositories hold") |

GitLab sets no limit on the number of a release's links; each link's permanent
path must be at most 128 characters of letters, digits and `_ . - /`, and its
name, address and path its own within the release, or the release is not created,
which the two jobs check first; 6.3.6's longest is 76. Measured on 28 September
2026 with the file sizes a release has (sizes of eight digits and more), by
`check_budget` over releases of 6.3.6's shape; on 29 September the source of the
AppImages' bundled libraries, its bundle and its copy added 1,094 bytes to
`releases.json` and 1,076 to `releases/latest`, the difference `check_budget`
measured with that file and without it; and the same day the ARM64 installer,
its SBOM, their bundles and the installer's `.sigstore.json` copy added 1,630
bytes to `releases.json` and 1,600 to `releases/latest` in the same way, and two
rules, 314 bytes, to `_redirects` (`/download/hMailServer-arm64.exe` and its
bundle); the table counts them.

GitLab's own `/-/releases/permalink/latest` is the release with the latest
`released_at` date. GitLab has no pre-release flag, so a pre-release, or the
`build-inputs-1` release, published after a stable one would take that link
over, and so would every link that names a file under it. The `/download/`
links cannot move that way. They change only when a stable release with a higher
version is published and the `pages` job runs.

The links are rules in `public/_redirects`, which GitLab Pages reads: one line
per link, `from to 302`, the target the file's address in the package registry.
Pages serves a file before a rule, so nothing is written under `public/download/`.
Pages answers only 301, 302 and 200, and only 301 and 302 with an absolute
target. It reads at most 1,000 rules and 64 KiB. It ignores a trailing slash, so
`/download` works too. The rules name the custom domain's paths, and they answer
the same at the project's own Pages address, GitLab's unique domain for it,
`hmailserver-94fa55.gitlab.io`. At `/_redirects` Pages shows each rule and
whether it is valid.

The links are for people and scripts. A server does not read them, because it
follows no redirect. They are HTTPS addresses like the rest of the site, which
has had its certificate since 26 September 2026 (issue #30).

A release is in the shape of GitHub's Releases API, which is what 6.2.28 and
every later version read, and it has only the fields the server reads:
`tag_name`, `name`, `html_url` (the release's page on GitLab), `published_at`
(UTC), `draft` (always false) and `prerelease`. From 6.3.6 a server keeps
`html_url` only as an `https://` address with a host and no space or control
character, since it shows the page as a link (the Control Deck's **Release
notes**, `Status.AvailableVersionUrl`); a feed or a mirror that gives anything
else still offers its release, with no page. Each file of the release is
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

### The Atom feed

`/releases.atom` is for people who follow releases in a feed reader, as
GitHub's feed of a project's releases was until 18 September 2026. It is an
Atom feed (RFC 4287) with one entry for each release in `releases.json`, in the
same order, newest first:

- the title is the release's name, or its tag when the name is blank;
- the link is the release's page on GitLab;
- the updated and published times are the time the release was made;
- a category says whether it is a release or a pre-release;
- the summary is the first paragraph of the release's own notes, as plain text,
  cut at 1,000 characters. The note a release re-hosted from GitHub opens with,
  and any heading, table or code, is skipped.

The summary comes from the release's notes on GitLab, not from `CHANGELOG.md`,
for three reasons. The feed is made from the releases alone, whichever commit
runs the script. A release re-hosted from GitHub or made by hand has no section
of its own there. And a release's notes are what its page shows. The feed's own
updated time is that of its newest entry, so the file changes only when a
release does.

An entry's id is `tag:updates.progressiverobot.com,2026:releases/<tag>`, a tag
URI made from the release's tag alone. A feed reader remembers the entries it
has shown by their ids. An id made from the release page's address would have
changed when the project moved into the group `hmailserver` on 29 September
2026, and every reader would have shown every release again. This one does not
change when the feed is made again, when a release is edited, or when the
project moves.

The site's pages link the feed, and name it for a browser's feed discovery.
GitLab also serves a feed of its own at `/-/releases.atom` under the project's
address. That one carries the whole notes, runs to a megabyte, and its ids are
the release pages' addresses, which the move changed. It also lists
`build-inputs-1`, which is not a server release.

`build/check-atom-feed.py` checks the feed against RFC 4287: the elements the
feed and every entry must have, RFC 3339 dates, and ids that are unique and in
the form above. It also holds the feed to `releases.json` and
`releases/latest`: an entry for each release, in order, with its name, page,
time and kind, so the newest entry is the newest release, and an entry for the
stable release `releases/latest` names. `--previous` compares an earlier copy,
and fails if one of its ids is missing. Its negative controls refuse each kind
of broken feed. The generator's `--self-test` runs the checker and its negative
controls, and `--serve-check` runs it against the feed the site serves.

How it is published
-------------------

`build/generate-update-feed.py` reads the releases and the package registry
through GitLab's public API, with no token, and writes the site into `public/`.
The `pages` job in `.gitlab-ci.yml` runs it on GitLab's shared runners. The
`package-repos` job then adds the signed apt and dnf repositories to the same
site, when their signing key is set ([PackageManagers.md](PackageManagers.md),
"apt and dnf"), and GitLab Pages deploys the result. The two jobs run for a push
to `master`, for a `v*` tag, and for a pipeline schedule on `master`, which runs
those two alone.
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
its bundle. It also follows `_redirects` as Pages does, a file first, and
checks the download links. Each name the stable release carries must answer a
302 to that release's own file, and `/download/` a 302 to its page. A name it
does not carry must not be redirected. A rule Pages would refuse fails, and so
does a stable release that is a pre-release. `--check --site` fails when the
deployed site is not what the releases say now, which is how to tell a stale
feed from a current one. It checks the download links by what they answer,
because Pages answers `/_redirects` with its page of the rules, not the file.
`--check` of a directory also lists `package-repos`' files, `apt/`, `rpm/` and
the repository key, as not in the releases.
`--self-test` needs no network.

The name is a CNAME, `updates` to `progressiverobot.gitlab.io`, served DNS-only
(not proxied) at Cloudflare. The domain is added and verified in the project's
Pages settings, and GitLab issues and renews its Let's Encrypt certificate. The
CNAME was made while the project was in the personal namespace, and it still
works since the move: GitLab Pages serves every site, a custom domain's
included, at the same addresses, and picks the site by the name asked for, not
by the `*.gitlab.io` name the CNAME goes through. On 29 September 2026, after
the move, `progressiverobot.gitlab.io` and `hmailserver.gitlab.io` both
resolved to 35.185.44.232 and 2600:1901:0:7b8a::, the domain was still
verified, and the site answered the feed at `updates.progressiverobot.com`.
Pointing the CNAME at `hmailserver.gitlab.io`, the group's own name, is tidier
and changes nothing a server sees; it is the owner's DNS change, and optional.

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
