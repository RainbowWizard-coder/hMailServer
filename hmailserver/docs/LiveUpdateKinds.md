The live update and where the server runs
==========================================

The live update has three steps: the check reads the release feed
(`docs/UpdateFeed.md`), the download fetches the release's own file and verifies
it against its Sigstore bundle, and the apply hands that file to the update
helper, which stops the service, installs it and starts the service again. The
file is the one the project's release carries for this kind of installation: the
installer on Windows, and on Linux the `.deb`, the `.rpm` or the AppImage the
server was installed from.

That is right when the project's own file is what was installed. It is wrong
when something else installed the server and keeps it up to date: a
distribution's package, a snap, or a container image. Such a server must never
replace itself with the project's file. From 6.3.6 it does not. The check still
says when a newer release exists, and names what updates the server instead.

*Written on 28 September 2026 for 6.3.6. The snap and the container were tested
by starting a built server with their environment (below); there is no snap of
hMailServer yet, and the image was not built for this document.*

How the server tells where it runs
----------------------------------

The server asks each time the check's status is read, in this order:

| Kind | How it is known | `liveUpdate` | `updateCommand` |
|---|---|---|---|
| `installer` | Windows, always | true | empty |
| `container` | `HM_INSTALL_KIND=container` in the environment, which the image's `Dockerfile` sets | false | `docker pull registry.gitlab.com/hmailserver/hmailserver:<release>` |
| `snap` | `SNAP` in the environment, which snapd sets for every process of a snap, naming the directory the server's own file is in | false | `snap refresh hmailserver` (the snap's instance name, from `SNAP_INSTANCE_NAME` or `SNAP_NAME`) |
| `distribution` | the server was built with `-DHM_LIVE_UPDATE=OFF` | false | the build's `HM_UPDATE_COMMAND`, which may be empty |
| `appimage` | `APPIMAGE` in the environment, which the AppImage runtime sets | true | empty |
| `deb` | dpkg's record `/var/lib/dpkg/info/hmailserver.list` exists | true | empty |
| `rpm` | `rpm -q hmailserver` succeeds on a machine without dpkg | true | empty |
| `none` | none of these: a build tree or a tarball | false | empty |

The container comes first because the image installs the `.deb`, so dpkg's
record is in the image too. A `SNAP` alone does not make a snap: a process passes
its environment on, and a terminal that is itself a snap gives its `SNAP` to
everything started from it. So the server's own file (`/proc/self/exe`) must be
inside the directory `SNAP` names, as it is in a snap's mounted revision; an
AppImage or a build tree started from such a terminal is what it would be
without the variable. `SNAP_INSTANCE_NAME` and `SNAP_NAME` are named only as
snapd spells a name - lower-case letters, digits, `-` and `_`, beginning with a
letter or a digit - and otherwise the command says `hmailserver`.
`HM_INSTALL_KIND` takes one value, `container`, in any case; any other value is
ignored and the server goes on down the table. The variable is not a setting, and
`hmailserver --apply-environment` does not take it for one.

`none` keeps what it always said. Its check reports the release, and the
download says the release carries nothing this installation could install.

What the server says
--------------------

**The check** reports the release as before (state 2, `availableVersion` and the
rest), and its line in the application log adds one sentence for a snap, a
container or a distribution's build. For a snap:

> Update check: hMailServer 6.3.7 is available (published ...); this server
> runs 6.3.6. ... This server runs as the snap hmailserver, which snapd updates:
> snapd refreshes its snaps on its own several times a day, and snap refresh
> hmailserver does it now. The server downloads and installs no release file
> itself.

**`GET /api/v1/update`** has four keys more than before. A client that does not
know them reads the rest as it always did.

| Key | Meaning |
|---|---|
| `installKind` | the kind above, `none` for none |
| `liveUpdate` | whether this server downloads and installs a release itself |
| `updateCommand` | the command that updates it, where something else does; otherwise empty |
| `updateAdvice` | the sentence the log line ends with; empty where the live update does it |

For a container offered 6.3.7:

```json
"installKind":"container","liveUpdate":false,
"updateCommand":"docker pull registry.gitlab.com/hmailserver/hmailserver:6.3.7",
"updateAdvice":"This server runs in a container, which is updated by replacing its image, ..."
```

**The download** (`POST /api/v1/update/download`, the scheduled download) fetches
nothing for a snap, a container or a distribution's build. The state stays 2,
and `lastError` is `updateAdvice`. Nothing is written
to the Updates folder. The scheduled download never asks, because the check
takes no file from the release for these kinds.

**The install** (`POST /api/v1/update/install`) refuses at once with the same
sentence, before it looks for a downloaded file. The state becomes 5, as for any
refused install, and `lastError` says why.

**The Control Deck's Update page** shows *Installed as* and *Update with* beside
the version and channel. Where `liveUpdate` is false it shows the server's
sentence in place of the note about draining, and no *Download and verify* or
*Install the update* button. The Windows Control Panel is unchanged, because a
Windows server is always the installer.

Building without the live update
--------------------------------

A distribution, a store or anyone whose package manager updates the server
builds it with the live update off, and names the command its users update with:

```sh
cmake -S hmailserver/source/Server -B build/linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DHM_LIVE_UPDATE=OFF \
      -DHM_UPDATE_COMMAND="apt upgrade hmailserver"
```

Other examples are `dnf upgrade hmailserver`, `zypper update hmailserver` and
`apk upgrade hmailserver`.

What the option leaves out:

- **The apply step is not compiled.** That is the hand-over to
  `hmailserver-update`, the rollback image and the drain around them
  (`UpdateInstaller.cpp`). `UpdateInstaller::Apply` only refuses, naming the
  command.
- **The fetch of a release file is not compiled** (`UpdateDownloader::FetchVerified`).
  The download refuses at its start.
- **`hmailserver-update`, `hmailserver-update.path` and
  `hmailserver-update.service` are not installed.** `cmake --install` and CPack
  leave all three out. The server and `hmailserver.service` are installed as
  before. `build-appimage.sh` leaves them out of an AppImage made from such a
  tree too, reading the option from its `CMakeCache.txt`. The CPack packages'
  maintainer scripts still try to enable and start
  `hmailserver-update.path`, and go on quietly when it is not there (`|| true`
  in the `.deb`'s, `|| :` in the `.rpm`'s).

What stays:

- the check, with the release, its date and its page, and the command;
- `hmailserver --verify-update`, which checks a file against its bundle and
  installs nothing.

`HM_UPDATE_COMMAND` may hold letters, digits, spaces and the characters
`._/:=+@,~-`. Configuring with any other character stops CMake with an error. The command is
compiled into the server as a string and shown to administrators, so it is kept
to characters that no shell, compiler command line or C string treats
specially. With the live update on, the command is not used, and CMake says so
in a warning.

The option is for the Linux build (CMake). The Windows server is built with
MSBuild and always carries the live update. A Windows build defined with
`HM_LIVE_UPDATE=0` stops at compile time.

A snap and a container
----------------------

Neither needs a build option. The kind is read from the environment when the
server runs, so the same binary is right in both. Neither ever downloads or
installs a release file.

- **A snap** is updated by snapd, which refreshes installed snaps on its own
  schedule, four times a day by default. `snap refresh hmailserver` does it at
  once.
- **A container** is updated by replacing its image. Pull the new tag and
  recreate the container: `docker compose pull` then `docker compose up -d`, or
  the image tag in the Helm chart's values. The data, log and configuration
  volumes carry over, and the new server upgrades the database schema at its
  first start (`HM_UPGRADE_DATABASE`, on by default). The command names the
  project's registry path with the release on offer as its tag. A server that
  knows of no newer release names `:latest`.

The image sets `HM_INSTALL_KIND=container` from 6.3.6.

What the live update did in an image before 6.3.6
-------------------------------------------------

The images built up to 6.3.5 have no mark, so their server takes itself for a
`.deb` installation. This was traced through the code of 6.3.5 on 28 September
2026. The image was not run, because none was built for this document.

1. **The check.** It is off by default (`UpdateCheckEnabled=0`). With it on, or
   with *Check now* in the Deck, it finds dpkg's record and asks the release for
   `hmailserver_<version>_amd64.deb`. The release carries that file, so the check
   offers it (state 2), and the Deck shows *Download and verify*.
2. **The download.** It is by hand unless `UpdateAutoDownload=1`. The server
   fetches the `.deb` and its bundle into `/var/lib/hmailserver/Updates`, which
   is the data volume, and verifies them. The state becomes 3, and the Deck
   shows *Install the update*.
3. **The install.** By hand, or in `UpdateWindow` if that is set:
   - The server finds `/usr/lib/hmailserver/hmailserver-update`, which the
     `.deb` installed into the image.
   - It fetches the running version's `.deb` as the rollback image, into
     `Updates/rollback`, where the feed still offers that release.
   - It drains: it closes the mail listeners and pauses delivery.
   - It then looks for a way to reach the helper and finds none.
     `RUNTIME_DIRECTORY` is not set, because no systemd unit started the server,
     so there is nowhere for the path unit to watch. `INVOCATION_ID` is not set
     either, so `systemd-run` is not tried.
   - It reports HM6890: *"This server cannot hand the update to a process that
     could apply it: RUNTIME_DIRECTORY is not set ..., and this server is not
     running under systemd ... install it the way this version was
     installed."*
   - It leaves the drain at once, so the listeners open again. The state
     becomes 5.

**Nothing was installed**, and the server kept running on the old version. The
cost:

- two packages of a few megabytes each were left in the data volume;
- the listeners were closed for a moment;
- HM6890 was written to the error log;
- the advice was wrong for a container. Installing a `.deb` inside the container
  is lost when the container is recreated, and the server runs as `hmailserver`,
  which cannot install a package.

With `UpdateAutoDownload=1` and an `UpdateWindow`, the unattended apply went the
same way, once the backup `UpdateBackupBeforeApply` (on by default) asks for had
been taken; with that setting on and no backup destination, the apply was never
attempted and the log said so. The round repeated every `UpdateCheckHours`
(24), and each round downloaded the new package again. On an image before
6.3.6, leave the live update's download and install alone and pull the new
image.

How it is tested
----------------

`build/test-install-kind.sh` starts a built server with nothing of the caller's
environment except each case's variables. The server reads a fake release feed
on the loopback, which offers a release far newer than any build with every
platform's file and bundle, and records each request. The script then asks the
REST API what the check says:

- **A snap** (`SNAP` naming the directory the server's file is in, `SNAP_NAME`,
  `SNAP_INSTANCE_NAME`) must report `snap`, `liveUpdate` false and
  `snap refresh hmailserver`, or `snap refresh hmailserver_edge` for a parallel
  instance.
- **A container** (`HM_INSTALL_KIND=container`) must report `container` and
  `docker pull registry.gitlab.com/hmailserver/hmailserver:<release>`.
- **In both**, the download and the install must refuse with `updateAdvice`.
  The feed must have been asked for the release and nothing else, and the
  Updates folder must hold no file.
- **Two controls** keep these checks honest. As an AppImage (`APPIMAGE`), the
  same server does fetch its file's bundle from the feed, so a fetch would be
  seen. With no marker, it reports `none`, so each kind comes from its marker
  and not from the machine.
- **With `--off-server`**, a build with `-DHM_LIVE_UPDATE=OFF` must report
  `distribution` and name the command it was built with, and still say
  `snap refresh` in a snap.
- **With `--install-off`**, `cmake --install` of that build into an empty
  `DESTDIR` must leave no `hmailserver-update` and no unit of it, and must leave
  the server and `hmailserver.service`. **With `--install-on`**, a default build
  must leave all three.
- **What is not a kind:** a `SNAP` naming a directory the server is not in (an
  AppImage started from a snap's terminal keeps its live update), snap names
  snapd never gives (`--devmode`, `hm;id`: the command names `hmailserver`),
  and an `HM_INSTALL_KIND` other than `container` (`appliance`: `none`), while
  `Container` in capitals is a container.

The `linux-live-update` job (`.gitlab/ci/live-update.yml`) runs
`build/ci/linux-live-update.sh` on linix. The script builds the OFF variant and
runs the test against it, against `linux-build`'s server and against the job's
PostgreSQL.

Not covered here
----------------

- **The project's own per-distribution packages.** These are the
  `.deb` files built for each Debian and Ubuntu release and the `.rpm` files for
  each Fedora, EL and openSUSE release (`build/ci/packages/`). They are built
  with the live update on. The check of a server installed from one offers the
  generic `.deb` or `.rpm` of the release. Once the project's apt and dnf
  repositories exist, those packages should be built with `-DHM_LIVE_UPDATE=OFF`
  and `HM_UPDATE_COMMAND` set to `apt upgrade hmailserver` or
  `dnf upgrade hmailserver`, because the repository then updates them.
- **The Arch `PKGBUILD`** builds with the live update on. The AUR's own package
  should set it off.
