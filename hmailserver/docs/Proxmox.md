Proxmox VE: the server in a container of its own
================================================

**Two scripts in the repository put Progressive Robot's fork of hMailServer into a
Proxmox VE container: one command on the Proxmox VE host makes an unprivileged
Debian 13 container and installs the release's own Debian 13 package in it, verified
against the release's Sigstore signatures, with PostgreSQL beside it, the schema made
and the Control Deck on HTTPS. This is the fork, not the original hMailServer
([ForkAndOriginal.md](ForkAndOriginal.md)), and the scripts are the project's own,
not the Proxmox VE Helper-Scripts project's, though they are laid out as that
project's are.**

| | |
|---|---|
| The files | `hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh` (run on the host) and `.../proxmox/install/hmailserver-install.sh` (run in the container) |
| The host | Proxmox VE 8 or 9 on amd64, as root |
| The container | unprivileged, Debian 13 from Proxmox VE's `debian-13-standard` template, 2 cores, 2 GiB of memory, 512 MiB of swap, a 16 GiB disk, DHCP on `vmbr0`, nesting on (see [Nesting](#nesting)) |
| What it installs | the release's `hmailserver_<version>-1~debian13_<arch>.deb` (amd64 or arm64), from 6.3.6 on; PostgreSQL 17 from Debian's archive, or a PostgreSQL server you name |
| Fetched from | `gitlab.com` (the scripts and the release), Debian's archive and Proxmox VE's template mirror; nothing from GitHub |

Why the project has its own
---------------------------

The Proxmox VE Helper-Scripts collection (`community-scripts/ProxmoxVE`) takes a new
script only through its testing repository, DevScripts, whose pull-request template
asks, among its requirements for a new application, that it be at least six months
old and have 600 or more GitHub stars (both read on 23 October 2026). The fork's first
release was on 12 June 2026, and its GitHub mirror is new, so the project keeps a
script of its own instead. The layout - `ct/<app>.sh` that makes the container and
`install/<app>-install.sh` that installs the application in it, the `var_*` names
for every answer, `update` inside the container - is that collection's, so that
anyone who knows it knows these. Its shared functions (`build.func`, `install.func`)
are not used: they are fetched from GitHub when a script runs, and the project's
scripts fetch nothing from there. If the fork later meets those requirements, a
submission is a separate piece of work, and these files are where it would start.

Making the container
--------------------

On the Proxmox VE host, as root:

```sh
bash -c "$(curl -fsSL https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh)"
```

It prints what it will make - the container's ID (the next free one), its name,
resources, storage, template and network, the install script it will run and the
database - and asks before it changes anything. Every answer is a variable, given in
front of the command, which also makes it run without the question:

```sh
var_ctid=120 var_hostname=mail var_ram=4096 var_disk=32 var_net=192.0.2.25/24 var_gateway=192.0.2.1 var_yes=yes \
  bash -c "$(curl -fsSL https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh)"
```

| Variable | Default | |
|---|---|---|
| `var_ctid` | the next free ID | refused when taken (`pvesh get /cluster/nextid --vmid`) |
| `var_hostname` | `hmailserver` | |
| `var_cpu`, `var_ram`, `var_swap`, `var_disk` | 2, 2048, 512, 16 | cores, MiB, MiB, GiB |
| `var_storage` | the first active storage that takes containers | `pvesm status -content rootdir` |
| `var_template_storage` | the first active storage that takes templates | `pvesm status -content vztmpl` |
| `var_brg`, `var_net`, `var_gateway`, `var_vlan`, `var_mtu`, `var_ipv6`, `var_ns` | `vmbr0`, `dhcp` | the container's `net0` and DNS server; a value holding a comma, which would add a key of its own to `net0`, is refused |
| `var_ssh_keys` | none | a file of public keys for root in the container (`pct create --ssh-public-keys`) |
| `var_nesting` | 1 | 0 makes the container without nesting |
| `var_onboot` | 1 | start the container with the host |
| `var_scripts_ref` | `master` | the branch or tag of this repository the install script, and later `update`, are taken from, for example `v6.3.8` once a release carries these files |
| `var_hm_version` | the newest release | for example `6.3.7`; 6.3.6 or later |
| `var_db` | `local` | `local`: PostgreSQL in the container; `external`: the server below |
| `var_db_host`, `var_db_port`, `var_db_name`, `var_db_user`, `var_db_password` | -, 5432, `hmailserver`, `hmailserver` | the external PostgreSQL |
| `var_rest_port` | 8443 | the REST API's and the Control Deck's port; 0 leaves them off |
| `var_dry_run` | no | `yes`, or `--dry-run` after the script, prints every command that would change the host and changes nothing |

The template is Proxmox VE's own `debian-13-standard`, the newest one `pveam
available` lists, downloaded with `pveam download`, which checks it against the
checksum in Proxmox's signed index. The container is made with `pct create ...
--unprivileged 1`; a privileged one is refused. The install script's variables reach
the container in a file of mode 0600, which the install script reads and deletes, so
a database password is never on a command line of the host or the container.

At the end the install script prints the administrator's password - once: it is in
no file and no log, and the configuration holds only its PBKDF2 hash - and the
Control Deck's address, `https://<the container's address>:8443/`. Sign in as
`Administrator`.

### Nesting

Nesting is on by default, because `hmailserver.service` is sandboxed by systemd
(`ProtectSystem=strict`, `PrivateTmp`, `PrivateDevices`, `ProtectProc=invisible` and
the rest, [PackagesDeb.md](PackagesDeb.md), "Started under systemd"), and Proxmox
VE's own documentation of the option (`pct(1)`, `nesting`) says that it "is also
required by systemd to isolate services". The Helper-Scripts collection turns it on
for the same reason, and its script warns that without it a Debian 13 container may
start with failing services. What it costs, in the same documentation's words: it
"will expose procfs and sysfs contents of the host to the guest". `var_nesting=0`
makes the container without it; that has not been tried, and the service may then
not start.

What the install does
---------------------

`install/hmailserver-install.sh` is copied into the container as
`/usr/local/sbin/hmailserver-lxc` and run there (it runs as well on any Debian 13
machine with systemd, as root: `hmailserver-lxc install`).

1. **Tools from Debian's archive:** curl, jq, OpenSSL and cosign (2.5 in Debian 13).
2. **The release:** `var_hm_version`, or the newest release of the project whose tag
   is `v<major>.<minor>.<patch>`, read from GitLab's releases API.
3. **The download, verified.** The release's Debian 13 package for the container's
   architecture, `hmailserver-linux-<version>-SHA256SUMS.txt`, and the
   `.sigstore.json` bundle of each, from the release's package registry, each of them
   one the release itself links. Each file is verified with `cosign verify-blob
   --new-bundle-format` as signed by the project's pipeline for that tag (identity
   `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`,
   issuer `https://gitlab.com`); the project's id, `86730559`, is read from each
   signing certificate's extension with OpenSSL, as SECURITY.md's "Verifying a
   release" does by hand; and the package's SHA-256 must be the one line the signed
   sums give it. A file that fails any of these stops the install before anything
   is installed.
4. **The package,** with `apt-get`, which takes its dependencies from Debian's
   archive.
5. **The database.** With `var_db=local`, PostgreSQL from Debian's archive, a role
   and an empty database of its own (no `CREATEDB`) and a generated password of 32
   letters and digits, given to `psql` on its standard input. With
   `var_db=external`, the server you name: make the role there first, and the
   database too unless the role may create one ([packaging/README.md](../source/Server/platform/packaging/README.md),
   "Install and get running", step 2).
6. **The configuration,** as that README's steps 3 to 5 and 7 do it by hand: the
   `[Database]` section of `/etc/hmailserver/hMailServer.ini`; the administrator's
   password, generated and given to `hmailserver --set-admin-password` on standard
   input; `hmailserver --check-config` and `--create-database` as the service's
   user; the stored-secrets key made and exported to
   `/root/hmailserver-stored-secrets.key` (mode 0600 - copy it out of the container
   and keep it with the database backups, [SecretsAtRest.md](SecretsAtRest.md)); and
   the REST API and the Control Deck on `var_rest_port`, on every address, over TLS
   with a self-signed certificate made for the container's names and addresses
   (`/etc/hmailserver/rest-api.crt`, whose SHA-256 fingerprint the install prints, to
   check against the browser's warning), set with `hmailserver --set-setting`. The
   server serves the REST API off the loopback only over TLS.
7. **The start:** `systemctl restart hmailserver`. Its first start makes the mail
   ports' own self-signed certificate and offers STARTTLS on 25, 587, 110 and 143
   ([SecureDefaults.md](SecureDefaults.md)). The install then asks the server: a
   `220` greeting on port 25 and STARTTLS in its EHLO, and `GET /api/v1/status`
   answered 200 with the administrator's password.

Nothing in the container's firewall is changed, and the container has none of its
own unless you give it one in Proxmox VE. Many providers block outbound port 25, and
a mail server that cannot reach other servers on it can only receive.

Updating
--------

Inside the container (`pct enter <ID>`):

```sh
update
```

`update` is a small script the host's script leaves in `/usr/bin`, as the
Helper-Scripts do: it fetches `ct/hmailserver.sh` from the repository at the ref the
container was made from, which, run inside its container, fetches the install script
of that ref and runs `hmailserver-lxc update`. `hmailserver-lxc update` alone does the
same with the copy already in the container. The update:

- does nothing when the newest release (or `var_hm_version`) is the installed one or
  older, unless `var_force=yes`;
- downloads and verifies the new release exactly as the install does;
- with the database in the container, writes a `pg_dump` of it to
  `/var/backups/hmailserver-lxc/hmailserver-<old version>-<time>.dump` first
  (`pg_restore` puts it back);
- stops the service, installs the package with `apt-get` - whose post-install step
  runs `hmailserver --upgrade-database` - and starts it, and asks port 25 and the
  REST API again (the status answers 401 without the password, which the update
  does not have).

A snapshot of the container (`pct snapshot <ID> before-update`) before an update is
the quickest way back from one that goes wrong.

The server's own live update ([UpdateFeed.md](UpdateFeed.md)) applies nothing by
itself unless `UpdateAutoDownload` is turned on, which the install does not do; it
installs the generic `.deb`, which is built on Ubuntu 26.04, not the Debian 13
package these scripts install, so in such a container use `update` instead.

What has run
------------

No Proxmox VE host was available, so neither script has run on one: the container's
creation is shown by a dry run, and the install by a stand-in for the container.
On 25 October 2026, on a Linux build machine of the project's (Docker 29, Debian 13
images by digest):

- **The install, in a stand-in for the container:** a Debian 13 container
  (`debian:trixie-slim`) with systemd 257.13 as PID 1, not privileged - given
  `CAP_SYS_ADMIN`, which systemd needs for the units' mount namespaces, and its own
  writable cgroup - with `systemd-binfmt` masked. `hmailserver-lxc install` with
  `var_hm_version=6.3.6` and the database local: both files verified as signed for
  `v6.3.6` with the project's id, the package held to the signed sums, PostgreSQL
  17.11 installed, the schema made, and the server answered `220 hmailserver
  ESMTP`, offered STARTTLS (which `openssl s_client -starttls smtp` then completed)
  and answered `GET /api/v1/status` with 200 (`6.3.6-B45`). From another machine the
  Control Deck answered `GET /` with its page over TLS, and the status 401 without a
  password. The service ran as `hmailserver` with `CAP_NET_BIND_SERVICE` alone, no
  new privileges and a seccomp filter, and systemd left no part of its sandbox out.
  The administrator's password was in no file under the container's `/etc`, `/root`,
  `/var/lib/hmailserver`, `/var/log`, `/tmp`, `/var/tmp` or `/usr/local`, and nowhere in
  its journal.
- **The update:** `ct/hmailserver.sh` run inside that container took the update
  path: 6.3.7 verified, the database dumped (844 KiB), the package upgraded (its
  post-install step found the schema at 6108, the version 6.3.7 needs), and the
  server answered again, `6.3.7-B46`. A second `update` said there was nothing to
  do.
- **The host's script, end to end, against stand-ins** of `pveversion`, `pvesh`,
  `pvesm`, `pveam` and `pct` that answer in Proxmox VE 9's form, `pct exec` and `pct
  push` acting on a fresh container of the same kind, with `var_db=external`
  pointing at a PostgreSQL 18 server in a container of its own: the container made
  with the arguments below, the install script pushed and run, 6.3.7 installed and
  answering on 25 and on `var_rest_port=9443`, `update` left in `/usr/bin`. The
  database password was in no file the scripts left.
- **The dry runs** (`--dry-run`), without stand-ins and with them: every command
  that would change the host printed and none run, the stand-ins' log holding only
  the questions (`pveversion`, `pvesh get /cluster/nextid`, `pvesm status`, `pveam
  list` and `pveam available`); a database password in no line. With the
  defaults the dry run prints:

  ```
  + pveam download local debian-13-standard_13.1-2_amd64.tar.zst
  + pct create 105 local:vztmpl/debian-13-standard_13.1-2_amd64.tar.zst --hostname hmailserver --cores 2 --memory 2048 --swap 512 --rootfs local-lvm:16 --net0 name=eth0\,bridge=vmbr0\,ip=dhcp --unprivileged 1 --ostype debian --onboot 1 --tags hmailserver\;mail --description Progressive\ Robot\'s\ fork\ of\ hMailServer\ \(https://gitlab.com/hmailserver/hmailserver\)\,\ made\ by\ its\ ct/hmailserver.sh --features nesting=1
  + pct start 105
  + pct push 105 /tmp/tmp.XXXXXXXXXX/hmailserver-install.sh /usr/local/sbin/hmailserver-lxc --perms 0755
  + pct push 105 /tmp/tmp.XXXXXXXXXX/install.env /root/.hmailserver-lxc.env --perms 0600
  + pct exec 105 -- bash /usr/local/sbin/hmailserver-lxc install --env-file /root/.hmailserver-lxc.env
  + pct push 105 /tmp/tmp.XXXXXXXXXX/update /usr/bin/update --perms 0755
  ```

- **The refusals:** an ID the host has, a `var_net` holding a comma, a privileged
  container, Proxmox VE 7, the script run without `--dry-run` on a machine that is
  not a Proxmox VE host; and, of the download's checks, each run against a file made
  to fail it: the package with one byte changed, the package checked as signed for
  another tag, 6.3.5's package (signed at the project's first path) checked at the
  group's, 6.3.3's bundle (signed by GitHub Actions, another repository id), sums
  with an altered line, without the package's line and naming it twice, and altered
  sums against the sums' bundle. Each was refused, and the real files passed.
- **shellcheck** 0.10.0 passes both scripts.

What it does not show: Proxmox VE's own `pct create` and `pct start` with these
arguments, the template's download, an unprivileged container's user namespace and
AppArmor profile (the stand-in has neither), `var_nesting=0`, an arm64 machine, and
`update` fetching the scripts from `master`, which holds them only once this change
has landed.
