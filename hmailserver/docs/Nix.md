The Nix flake: a package, a NixOS module and a NixOS test
=========================================================

**The repository is a Nix flake. It builds the Linux server as a Nix package with
nixpkgs' own libraries, gives NixOS a module, `services.hmailserver`, that runs it
under systemd against a local PostgreSQL, and carries a NixOS test that starts that
machine and asks SMTP and IMAP for their greetings. Nothing is submitted anywhere
for it to be used: a flake in the project's repository is a channel of its own, and
`gitlab:hmailserver/hmailserver` is its name. It downloads nothing from GitHub:
nixpkgs is NixOS's own release tarball, pinned by its hash.**

| | |
|---|---|
| The files | `flake.nix` and `flake.lock` at the top of the repository; `nix/package.nix` (the package), `nix/module.nix` (the NixOS module), `nix/test.nix` and `nix/probe.sh` (the NixOS test) |
| nixpkgs | NixOS 26.05's release tarball from `releases.nixos.org` (`nixos-26.05.10882.7fc6f2c20af0`, where `channels.nixos.org/nixos-26.05` pointed on 29 September 2026), locked by its NAR hash |
| Outputs | `packages.<system>.hmailserver` (and `default`), `overlays.default`, `nixosModules.default` (and `hmailserver`), `checks.<system>.hmailserver` and `checks.<system>.nixos`, for `x86_64-linux` and `aarch64-linux` |
| CI | the `nix` job (`.gitlab/ci/nix.yml`, `build/ci/nix.sh`) on linix, for master and `batch*` |

**Where it stands (21 October 2026):** see [What has run](#what-has-run). The flake
is in the tree from 6.3.6, and the release tags `v6.3.6` (made on 5 October 2026)
and `v6.3.7` carry it, so `gitlab:hmailserver/hmailserver/v6.3.6` and `.../v6.3.7`
name a flake; an earlier release carries none. `nix build` of either tag, fetched
from GitLab, has not been run yet: the `nix` job builds the checkout of a `master`
or `batch*` pipeline.
It is not nixpkgs: NixOS's own package set takes packages only as pull
requests on GitHub, which the project does not use, so its row was taken off
Roadmap3 on 13 October 2026 (Roadmap3, "Rows removed").

Using it
--------

The flakes feature has to be on (`--extra-experimental-features 'nix-command
flakes'`, or `experimental-features = nix-command flakes` in `nix.conf`). The
project also turns the flake registry off (`--option flake-registry ''`), because
the registry is a file that names GitHub; nothing below needs it.

Run the server of a release without installing anything:

    nix run gitlab:hmailserver/hmailserver/v6.3.7 -- --version

Build it from a checkout:

    nix build .#hmailserver
    ./result/bin/hmailserver --version

The package holds:

| Path | What it is |
|---|---|
| `bin/hmailserver` | the server |
| `bin/dh2048.pem` | the data file the server reads from its own directory, as the `.deb` puts it in `/usr/bin` |
| `share/hmailserver/` | `DBScripts`, the Control Deck (`WebAdmin`) and the licences: the `ProgramFolder` |
| `lib/hmailserver/autoban-hook` | the auto-ban firewall hook, which the server runs from here: its build is told where it installs it (`autoBanFirewall`, below) |
| `etc/hmailserver/hMailServer.ini` | the packaged configuration, its `ProgramFolder` pointing into the package: what an administrator copies |
| `etc/logrotate.d/`, `etc/fail2ban/` | the logrotate rule and the fail2ban filter and jail, as examples |

`hmctl`, the command-line client, is the package's second output, `cli`
(`nix build '.#hmailserver^cli'`), because it is Python: the server's closure does not
carry an interpreter, as the Debian source package keeps it in `hmailserver-cli`.

### On NixOS

A system flake names this one as an input and imports the module:

```nix
{
  inputs.hmailserver.url = "gitlab:hmailserver/hmailserver/v6.3.7";

  outputs = { nixpkgs, hmailserver, ... }: {
    nixosConfigurations.mail = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        hmailserver.nixosModules.default
        {
          services.hmailserver = {
            enable = true;
            openFirewall = true;
            adminPasswordFile = "/run/secrets/hmailserver-admin";
            settings.RestApiPort = 8045;
          };
        }
      ];
    };
  };
}
```

With the defaults, that is all: PostgreSQL runs on the same machine, and the
server reaches it over its Unix socket as the role of its own account, which peer
authentication lets in with no password. The first start makes the database, and
secures it as the packaged unit's first start does
([SecureDefaults.md](SecureDefaults.md)): a self-signed certificate, STARTTLS on 25,
587, 110 and 143, and a password refused without TLS from any address but the
machine's own.

| Option | Default | What it is |
|---|---|---|
| `enable` | `false` | |
| `package` | this flake's package | |
| `user`, `group` | `hmailserver` | the account the server runs as, and its group, which may read the configuration |
| `dataDir` | `/var/lib/hmailserver` | the message store and, beside it, the stored-secrets key: back it up with the database |
| `logDir` | `/var/log/hmailserver` | |
| `database.type` | `PostgreSQL` | or `MySQL`, which covers MariaDB through the connector the package opens |
| `database.host` | `/run/postgresql` | a host name, or for PostgreSQL the directory of its socket |
| `database.port` | `5432` | |
| `database.name`, `database.user` | `hmailserver` | |
| `database.passwordFile` | none | a file holding the database password, read as root at each start; none for peer authentication, and then the configuration's `Password` is left as it is |
| `database.createLocally` | `true` | run PostgreSQL here, with a role for the server's account that may create its database |
| `adminPasswordFile` | none | a file holding the administrator password, which gates the REST API, hashed into the configuration at each start |
| `settings` | `{ }` | settings stored in the database with `hmailserver --set-setting` at each start; not for secrets |
| `openFirewall` | `false` | opens 25, 110, 143, 465, 587, 993 and 995 |
| `autoBanFirewall` | `false` | blocks each address the server auto-bans in the firewall for as long as the ban lasts: stores `AutoBanFirewall=1` and gives the service `CAP_NET_ADMIN` and `nft` (below) |

Name the two password files as strings, as the example does (`"/run/secrets/..."`,
where a secrets tool such as sops-nix or agenix puts them): a Nix path literal
(`./db-password`) is copied into the Nix store, which every account on the machine
can read.

What the module does, and why each part is as it is:

- **The configuration** is `/etc/hmailserver/hMailServer.ini`, 0640 and
  `root:hmailserver` in a 0750 directory, as the `.deb` installs it, so that
  `hmailserver`'s own commands (`--set-setting`, `--set-admin-password`,
  `--create-stored-secrets-key`, `--check-config`) find it with no `--config`. The
  first start writes the package's configuration there; every start, as root, sets
  the keys of `[Directories]` and `[Database]` the options name to the options
  (`nix/ini-merge.awk`) and keeps every other line: `[Security]`, where
  `--set-admin-password` writes the administrator's hash and
  `--create-stored-secrets-key` the stored-secrets key, `[Settings]`, and the other
  keys of `[Database]` (`PostgreSQLSslMode`, say). An edit to one of the keys the
  options name lasts until the next start. The database password is one of them
  only when `database.passwordFile` gives it; without one, the `Password` the file
  holds is kept.
- **root may use the database role**, through a peer map the module adds to
  PostgreSQL (`hmailserver root hmailserver`), so the administrator's commands run
  as the packaging guide gives them. The first of them after the first start
  (`platform/packaging/README.md`, step 4c):

        sudo hmailserver --create-stored-secrets-key
        sudo hmailserver --export-stored-secrets-key /root/hmailserver-stored-secrets.key

  and keep the exported file with the database backups, apart from them.
- **The settings** live in the database, as on every other platform: the
  configuration's `[Settings]` lines are what a first start adopts into the
  settings store, and from then on the store holds them. `settings` stores its
  values with `--set-setting`, as root, before each start, so the module's values
  win.
- **Before the server starts** - after `postgresql.target`, which NixOS reaches
  once `postgresql-setup.service` has made the role - as the service's account,
  `--create-database`,
  which makes the database and its schema the first time and does nothing after,
  then `--upgrade-database`, as the container's entrypoint runs both; as root, the
  `settings`, since `--set-setting` writes the server's copy of a setting into the
  configuration too and refuses an account that cannot write it; then, as the
  account again, the unit's `--apply-new-install-defaults --if-marked`.
- **The sandbox** is the packaged unit's (`platform/packaging/hmailserver.service`),
  setting for setting: `CAP_NET_BIND_SERVICE` alone, `ProtectSystem=strict` with
  the data and log directories writable, `UMask=0027`, and the rest; the unit's own
  comments say why each is there and why the others are not. `autoBanFirewall` adds
  `CAP_NET_ADMIN` (below).

### The auto-ban below the server

With `AutoBanFirewall=1` the server runs its auto-ban hook, `autoban-hook`, for each
address it bans and again when the ban ends; the hook keeps an nftables set with a
timeout (`platform/packaging/autoban-hook`). `autoBanFirewall = true` stores that
setting, puts `nftables` on the service's `PATH` and adds `CAP_NET_ADMIN` to its
ambient and bounding capabilities, which the hook needs to change the firewall. The
same two are given when `settings` sets `AutoBanFirewall` to 1. The packaged unit
leaves that capability to a drop-in, because a mail server that can rewrite the
host's firewall is a bigger grant than one that can bind port 25; here the option is
that deliberate step. An ambient capability passes to every program the service
starts, so whatever `AutoBanCommand` names, when it names anything, runs with it too.
Set over the REST API or the Control Deck instead, the setting finds the hook
without the capability: the error log has `HM6523`, the hook having exited with
code 1, the service's journal has `nft`'s "Operation not permitted", and
`autoban-hook check` run as the service's account names the capability that is
missing.

Up to 6.3.5 the server ran the hook from `/usr/lib/hmailserver/autoban-hook`
whatever prefix it was built for, which NixOS does not have, so the setting found no
hook. Two ways were weighed: a `systemd.tmpfiles` link at that path to the
package's hook, and the hook's path as a setting in the database. Neither was
taken. The build now tells the server where it installs the hook - `CMakeLists.txt`
compiles `CMAKE_INSTALL_PREFIX/lib/hmailserver/autoban-hook` into it, which is the
packages' `/usr/lib/hmailserver/autoban-hook` and the Nix store's path here - so the
server runs the hook of its own package: nothing is written outside the store, the
path cannot be changed at run time (a setting naming a program the server runs with
`CAP_NET_ADMIN` would be one more thing the REST API could aim), and a build
installed under `/usr/local` finds its hook too, which it did not. fail2ban with the
package's filter and jail (`etc/fail2ban/`) is the other way, and needs neither.

### Updating

The package cannot update itself: it is built with the live update off
(`-DHM_LIVE_UPDATE=OFF`), and its update check names the command that takes a
newer release into a NixOS configuration whose flake names this one as its input
`hmailserver`: `nix flake update hmailserver`, then `nixos-rebuild switch`. With the
input pinned to a release's tag, move the tag in the URL first. A different command
can be named by overriding the package's `updateCommand`.

How the package is built
------------------------

`nix/package.nix` runs the CMake build that `build/ci/linux-build.sh` runs, as a
distribution builds it (the Debian source package and the RPM spec build the same
way, [SourcePackages.md](SourcePackages.md)):

- **The distribution's libraries:** nixpkgs' Boost (shared: `-DHM_BOOST_STATIC=OFF`),
  OpenSSL, `libpq`, OpenLDAP and zlib; iconv is glibc's. QuickJS is compiled from the
  copy in the tree, as everywhere.
- **The live update off**, naming the command above (`HM_UPDATE_COMMAND`).
- **The source** is what the CMake build reads and nothing else (`lib.fileset` in
  `flake.nix`): `hmailserver/source/Server`, `DBScripts`, `hmctl`, the Deck,
  `dh2048.pem` and `LICENSE`. A change to the documents or the CI does not make a new
  package. A path named there that the tree no longer has stops the evaluation
  (`lib.fileset.unions`), so a file the CMake build stops reading leaves this list
  with it: `tlds.txt`, which 6.3.6 retired, did.
- **`checkPhase`** runs the server's `--version` - every static constructor runs
  before `main`, so a server that cannot start fails the build - and then every
  `*-tests` program the build makes, found rather than listed, as
  `linux-build.sh` finds them.
- **The install** writes the configuration, the logrotate rule and the fail2ban
  files to absolute paths under `/etc`, which a Nix build cannot write, so it
  installs into a staging directory and the package takes the prefix's part, and
  the configuration and the rest under `etc/` as examples. The systemd unit, which
  the install puts under the prefix when the prefix is not `/usr`
  (`HM_SYSTEMD_UNIT_DIR`, [SourcePackages.md](SourcePackages.md), "Another prefix"),
  is taken out of the package again: it names `/usr/bin/hmailserver`, and the module
  declares its own.
- **The MySQL and MariaDB client** is opened with `dlopen("libmariadb.so.3")` when
  such a database is configured, and not linked. A Nix binary finds a library only
  on its RUNPATH, so nixpkgs' MariaDB connector's directory is added to it.

Nothing is fetched from GitHub
------------------------------

- **nixpkgs** is `inputs.nixpkgs.url`, NixOS's own release tarball
  (`https://releases.nixos.org/nixos/26.05/<release>/nixexprs.tar.xz`, the file
  `https://channels.nixos.org/nixos-26.05/nixexprs.tar.xz` redirects to), never
  `github:NixOS/nixpkgs`. The release's own address, not the channel's, because the
  channel moves every few days and the lock's hash would stop matching it. It is
  taken with `flake = false` and imported, so neither nixpkgs' own `flake.nix` nor
  the flake registry is read.
- **Everything else** is substituted from `cache.nixos.org`, signed with
  `cache.nixos.org-1`. A dry run (`nix build --dry-run .#hmailserver`) on 29
  September 2026 listed 112 paths to fetch and one derivation to build,
  `hmailserver-6.3.5`. A derivation the cache does not hold would be built, and a
  fetcher inside it may name GitHub, so the CI job runs with GitHub's hosts at
  `0.0.0.0` and `::` (`.no-github`), where such a fetch fails by name.
- **`build/ci/nix.sh`** refuses a `flake.lock` whose inputs are not tarballs from
  `releases.nixos.org`.

To move nixpkgs to a newer release, read where
`https://channels.nixos.org/nixos-<release>/nixexprs.tar.xz` redirects (`curl -sI`),
put that address in `flake.nix`, and run `nix flake lock` (or `nix flake update`)
with the registry off; then the package's build and the NixOS test.

Where the NixOS test runs
-------------------------

`checks.x86_64-linux.nixos` (`nix/test.nix`) boots one NixOS machine in QEMU with
`services.hmailserver` and its PostgreSQL, the administrator password from
`adminPasswordFile` and `RestApiPort` from `settings`. It waits for the service,
then `nix/probe.sh` asks SMTP for its greeting (220), for STARTTLS in its EHLO
answer and for its QUIT (221), and IMAP for its greeting (`* OK`), as the Debian
package's autopkgtest asks; the REST API answers `/api/v1/status` with the
password; the configuration is 0640 `root:hmailserver` in a 0750 directory; root
makes the stored-secrets key with `hmailserver --create-stored-secrets-key`; the
service has not been restarted once (`NRestarts` 0: its first start waited for the
role PostgreSQL's setup makes); and all of it again after a restart, which sets
the configuration's directories and database anew over a database that is already
there, and must leave the key as it was, and a database password typed into the
file by hand. Then the auto-ban (`autoBanFirewall`, which the test machine sets):
`/usr/lib/hmailserver/autoban-hook` does not exist, the package's hook does, the
unit's ambient capabilities hold `cap_net_admin`, and the hook run as the service's
account without it is refused; an expiring range named `Auto-ban: 192.0.2.7` is
made through the REST API, as the auto-ban makes one, and within the minute the
server takes to reconcile, 192.0.2.7 is in the hook's set `inet hmailserver
autoban4`; the range deleted, it leaves the set.

A NixOS test wants KVM. **linix has none**: it is a virtual machine itself, with no
`/dev/kvm` and no `vmx` or `svm` flag (29 September 2026), and its runner passes no
device into a job; nor has s1-linux, a virtual machine on server1. Under QEMU's TCG
on linix the test's PostgreSQL timed out at boot on 13 October 2026 and failed the
job (17129125235), where server2 passed it in about 20 minutes. So since then the
`nix` job asks for the tags `linix` and `kvm`, which server2 alone carries - its
machine has `/dev/kvm`, and the owner gives its runner (57072507) the tag - and it
stays on the project's runners whatever the switches that send other jobs to
GitLab's hosted runners say ([ContinuousIntegration.md](ContinuousIntegration.md),
"GitLab's hosted runners beside the project's"). Until server2's runner passes the
device into its jobs (`devices = ["/dev/kvm"]` under `[runners.docker]` in its
`config.toml`), the job runs the test there under QEMU's TCG, which needs no KVM and
is about ten times slower (`HM_NIXOS_TEST_TCG=1`: Nix is told the machine has the
`kvm` feature the test asks for, and QEMU, finding no KVM, falls back to TCG), and
server2's CPUs carry in minutes. With the device, it runs with KVM and nothing else
changes: `build/ci/nix.sh`
opens the job's own device node to Nix's build users, who are not in the host's
`kvm` group - without that, QEMU is refused KVM and falls back to TCG without a
word, which is what the first run below did. By hand, with KVM:

    nix build -L .#checks.x86_64-linux.nixos

What has run
------------

On 29 September 2026, from `batch42` with this item's changes (Nix 2.35.2 in the
pinned `nixos/nix` image, nixpkgs `nixos-26.05.10882.7fc6f2c20af0`):

- **The lock.** `nix flake lock` with the registry off locked the tarball's NAR hash
  (`sha256-6AXgcEB8kiYLV1aewneIGmpljLLQcd2Xo/gmDYxvKYc=`). A dry run of the package
  listed 112 paths to fetch from `cache.nixos.org` and one to build; a dry run of the
  NixOS test listed 255 derivations to build, every one the package or a file NixOS
  writes (units, `/etc` entries, the test driver), none a fetch.
- **`nix flake check --no-build --all-systems`**: every output of both systems
  evaluates.
- **`nix build .#hmailserver`**, in the image: on linix, under the pipeline's load,
  from the tree before the musl changes (in about an hour), and in WSL from the final
  tree, twice. The server built with nixpkgs' GCC 15.3, Boost 1.89, OpenSSL 3.6,
  libpq 18.6 and OpenLDAP 2.6; `checkPhase` ran the server's `--version` (6.3.5 build
  44) and the eight testers, all passing; the package's references are glibc,
  libstdc++, OpenSSL, OpenLDAP, libpq, zlib, Boost, bash (for `autoban-hook`) and the
  MariaDB connector the RUNPATH names.
- **The NixOS test**, in WSL on the laptop (Docker, the same image): with `/dev/kvm`
  open to the build users, the test script took 39 s and passed; without KVM, under
  TCG, 435 s and passed. The first run with the device passed in used TCG all the
  same, which is why `nix.sh` now opens the device (above).
- **`build/ci/nix.sh`, the job's own script**, in WSL in a container without
  `/dev/kvm`, on a shallow clone as the runner makes (`GIT_DEPTH` 1), with
  `.no-github`'s hosts and the job's `NIX_CONFIG`: the lock check, the evaluation,
  the package in 30 minutes on four cores and the NixOS test under TCG in 407 s -
  37 minutes in all, which the job's three hours hold with room on a loaded linix.
  Run again on a shallow clone of the committed branch: the package in 27 minutes,
  the test in 302 s, 32 minutes in all, and the `cli` output (`^cli`) built.
- **The same script on linix**, the job's own runner, in the same image with no
  `/dev/kvm`, on a shallow clone of the final commit, beside `batch42`'s pipeline
  and the Alpine suite: the package in 22 minutes and the NixOS test under TCG in
  294 s, 27 minutes in all (30 September 2026, 00:17 UTC).
- **Two defects the test found in the module, fixed before this**: `--set-setting`
  refuses an account that cannot write the configuration, so the `settings` step
  runs as root; and the first design wrote the configuration whole at each start,
  which would have thrown away the stored-secrets key `--create-stored-secrets-key`
  writes into `[Security]` - the test now makes the key and holds the restart to it.
- **The re-check, 30 September 2026**, on the tree rebased onto `batch42`'s
  `c97fa7f03` (6.3.6's Public Suffix List, SURBL, backup and live-update changes, and
  eleven tester programs). The flake did not evaluate: `lib.fileset` named
  `tlds.txt`, which 6.3.6 retired, and `lib.fileset.unions` refuses a path that is
  not there. With that line gone and the module's two fixes (a database password
  typed into the configuration is kept when no `passwordFile` gives one; the unit
  waits for `postgresql.target`), the job's script passed end to end on linix, in
  its image without KVM: the evaluation of both systems, the package in 1413 s with
  `checkPhase` running the server's `--version` and all eleven testers, the NixOS
  test under TCG in 334 s with its two new checks (`NRestarts` 0 before the restart;
  the typed password and the configuration's mode kept across it), and the `cli`
  output. In the test's journal PostgreSQL's setup finished before the service
  started.

- **The auto-ban, 1 October 2026**, the job's script on the tree with
  `autoBanFirewall` and the hook's path from the build, with the job's
  `NIX_CONFIG`. On linix, without KVM: the evaluation of both systems and the
  package (4691 s, under a load of 20 to 40) passed, and the NixOS test did not
  finish - under TCG on that load the machine was still in its first boot,
  `initdb` among it, when the test's hour ran out, before its first check. In
  WSL on the laptop, with KVM, the tree, Nix's store and its build directory on
  a loop-mounted ext4 image (`df` in the container showed `/src` and `/nix` on
  `/dev/loop0`): the package in 1915 s, with `--version` and all fourteen
  testers, and the NixOS test with KVM in 189 s, PASSED - the address in the
  set 59 s after the range was made (`192.0.2.7 timeout 30m`, the set's ports
  25, 110, 143 and 587), and out of it 0.06 s after the range was deleted. The
  same run on the tree with `AutoBanFirewall.cpp` as it was, naming
  `/usr/lib/hmailserver/autoban-hook`, built (1542 s) and FAILED the test where
  it must: the address never reached the set, which never existed (`nft`
  answered "No such file or directory" at every try), and the wait timed out
  after 181 s. A first pair of WSL runs had the same results with the store on
  WSL's own ext4, where a loop mount made in an earlier `wsl` call had not
  held; they are not counted.

In `batch42`'s pipeline 2896082335 (30 September 2026) the `nix` job passed on
linix in 1,802 s: the flake's inputs, its evaluation, the package in 1,388 s with
its checks, and the NixOS test under TCG in 352 s, SMTP with STARTTLS and IMAP
answering. In 2897966924 it was cancelled with the pipeline.

Not yet run: `nix build gitlab:hmailserver/hmailserver/v6.3.6` (or `v6.3.7`), from
the tag itself. The tags exist, with the flake in them.

What is left
------------

- **A build from the tag.** `nix build gitlab:hmailserver/hmailserver/v6.3.6` is the
  row's test. The tag has existed since 5 October 2026, with the flake in it, and
  `v6.3.7` since 17 October; neither has been built from GitLab yet.
- **nixpkgs itself** cannot be done: its only route is a pull request to
  NixOS/nixpkgs on GitHub, and the NUR is on GitHub too, so on 13 October 2026
  its row was taken off Roadmap3, as the owner decided for what needs GitHub and
  has no GitLab form (Roadmap3, "Rows removed").
- **A binary cache.** Nobody's cache holds the package, so every user builds the
  server from source (half an hour on four cores). A cache of the project's own
  would be a spend and a signing key, the owner's.
- **aarch64-linux** evaluates (the job checks every output of both systems) and is
  built nowhere.
