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
| The host | Proxmox VE 8 or 9, as root, on amd64: the script makes amd64 containers only and has not been tried on any other kind of host |
| The container | unprivileged, Debian 13 from Proxmox VE's `debian-13-standard` template, 2 cores, 2 GiB of memory, 512 MiB of swap, a 16 GiB disk, DHCP on `vmbr0`, nesting on (see [Nesting](#nesting)) |
| What it installs | the release's `hmailserver_<version>-1~debian13_<arch>.deb` (amd64 or arm64), from 6.3.6 on; PostgreSQL 17 from Debian's archive, or a PostgreSQL server you name. The template's Postfix, which holds port 25, is removed |
| Fetched from | `gitlab.com` (the scripts and the release), Debian's archive, Proxmox VE's template mirror, and Sigstore's trust root from `tuf-repo-cdn.sigstore.dev`, which cosign fetches to verify the release's signatures; nothing from GitHub |

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
database - and whether this is a dry run or a run that changes the host, and asks
before it changes anything. It can ask only at a terminal: run without one (from a
pipe, or over `ssh` without `-t`) it stops there and changes nothing, unless it is
given `var_yes=yes`. Every answer is a variable, given in front of the command;
`var_yes=yes` makes it run without the question, at a terminal or not:

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
| `var_dry_run` | no | `yes` prints every command that would change the host and changes nothing |

A dry run, either way:

```sh
var_dry_run=yes bash -c "$(curl -fsSL https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh)"
bash -c "$(curl -fsSL https://gitlab.com/hmailserver/hmailserver/-/raw/master/hmailserver/source/Server/platform/packaging/proxmox/ct/hmailserver.sh)" _ --dry-run
```

`bash -c` gives the first word after the script to the script as its `$0`, not as
an argument, which is what the `_` is for; the script reads a `$0` that begins with
`-` as an argument as well, so that `--dry-run` and `-h` there do what they say and
any other such word is refused rather than ignored.

The template is Proxmox VE's own `debian-13-standard`. If the template storage has
one already, the newest there is used as it is: nothing is downloaded, and it may
be older than the newest Proxmox VE offers. If it has none, then after the summary
and the question, if one is asked, the script refreshes the host's list of templates (`pveam update`, which fetches
Proxmox's signed index) and downloads the newest one offered with `pveam download`,
which checks it against the checksum in that index. The container is made with
`pct create ... --unprivileged 1`; a privileged one is refused. The install
script's variables reach the container in a file of mode 0600, which the install
script reads and deletes, so a database password is never on a command line of the
host or the container.

At the end the install script prints the administrator's password - once: it is in
no file and no log, and the configuration holds only its PBKDF2 hash - and the
Control Deck's address, `https://<the container's address>:8443/` (an IPv6 address
in brackets). Sign in as `Administrator`. If a step after the password was set
fails, the password is printed with the failure instead ([When the install
stops](#when-the-install-stops)).

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

1. **Tools from Debian's archive:** curl, jq, OpenSSL, cosign (2.5 in Debian 13) and
   iproute2.
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
   is installed. cosign fetches Sigstore's trust root from
   `tuf-repo-cdn.sigstore.dev` for this, so the container must be able to reach it.
4. **Other mail servers and the ports.** Proxmox VE's `debian-13-standard` template
   comes with Postfix, running and listening on port 25 of the loopback; with it
   there, the server would start without port 25 and answer on the others. Postfix
   or Exim, if installed, has its systemd units stopped and is removed with
   `apt-get remove`, which keeps its configuration (`apt-get install postfix` puts
   it back), and the install says so, and whether it held port 25. If its queue
   holds mail, the install stops instead, with nothing removed. Any other installed
   package that provides `mail-transport-agent` (sendmail, OpenSMTPD, nullmailer,
   dma, msmtp-mta and the rest) stops the install, named, with nothing removed: the
   install does not read its queue, so deliver what it holds, remove it yourself and
   run the install again. Then the ports the server takes - 25, 110, 143, 587 and
   `var_rest_port` - must be free: if anything listens on one, the install stops,
   naming it, before the package is installed.
5. **The package,** with `apt-get`, which takes its dependencies from Debian's
   archive.
6. **The database.** With `var_db=local`, PostgreSQL from Debian's archive, a role
   and an empty database of its own (no `CREATEDB`) and a generated password of 32
   letters and digits, given to `psql` on its standard input. With
   `var_db=external`, the server you name: make the role there first, and the
   database too unless the role may create one ([packaging/README.md](../source/Server/platform/packaging/README.md),
   "Install and get running", step 2).
7. **The configuration,** as that README's steps 3 to 5 and 7 do it by hand: the
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
8. **The start:** `systemctl restart hmailserver`. Its first start makes the mail
   ports' own self-signed certificate and offers STARTTLS on 25, 587, 110 and 143
   ([SecureDefaults.md](SecureDefaults.md)). The install then asks the server: that
   every socket listening on port 25 belongs to the service's user, `hmailserver`
   (`ss -e` gives each socket's owner), so that another mail server's greeting is
   not taken for the server's; a `220` greeting there and STARTTLS in its EHLO; and
   `GET /api/v1/status` answered 200 with the administrator's password.

Nothing in the container's firewall is changed, and the container has none of its
own unless you give it one in Proxmox VE. Many providers block outbound port 25, and
a mail server that cannot reach other servers on it can only receive.

### When the install stops

The install stops at the first step that fails, with `HMAILSERVER-LXC FAILED:` and
the reason. If the administrator's password had been set (step 7), it is printed
with the failure, once, as it would have been at the end; if it is lost, `hmailserver
--set-admin-password`, run as root, sets another (it reads it from standard input).

On a Proxmox VE host, whatever the failure, the simplest way to start again is to
remove the container (`pct stop <ID>` and `pct destroy <ID>`), fix the cause and run
the host's command again, with the same variables. The host's script stops when the
install does, before it leaves `update` in the container, and the variables it passed
on are gone from the container, since the install deletes their file once it has
read them.

To start again in the same container, or on another Debian 13 machine:

- **A failure up to and including step 4** leaves nothing of the server installed
  (though step 4 may have removed Postfix or Exim, as it says). Fix the cause and
  run the install again, giving it its variables again in front of it, as root:
  `var_db=external var_db_host=db.example.org var_db_password_file=/root/db-password
  hmailserver-lxc install`, for example, or `hmailserver-lxc install` alone for the
  defaults. `var_db_password_file` names a file whose first line is the password,
  which keeps it off the command line.
- **A failure from step 5 on** leaves the package installed, and `hmailserver-lxc
  install` refuses to run again while it is. Nothing has reached the mailboxes yet,
  so remove what the install made and run it again as above: `apt-get purge
  hmailserver` (which leaves `/var/lib/hmailserver` and `/var/log/hmailserver`, as
  it says), `rm -rf /var/lib/hmailserver /var/log/hmailserver
  /root/hmailserver-stored-secrets.key /etc/hmailserver-lxc.conf`, and, with the
  database in the container, its role and database (`runuser -u postgres -- dropdb
  hmailserver` and `runuser -u postgres -- dropuser hmailserver`); with an
  external one, drop and make again the database the install wrote its schema to.

In a container the host's script made, `hmailserver-lxc update` does what `update`
would have, with the install script already in the container.

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
same with the copy already in the container. `update` asks no question - running it
is the yes - and its arguments go on to `ct/hmailserver.sh`: `update --dry-run` (or
`var_dry_run=yes update`) fetches the scripts and prints the two commands it would
run, the install script put in place and `hmailserver-lxc update`, and runs neither.
The update:

- does nothing when the newest release (or `var_hm_version`) is the installed one or
  older: it installs only a newer release, since `apt-get` installs nothing over
  the same version and refuses an older one, and an older server need not accept the
  newer one's schema;
- downloads and verifies the new release exactly as the install does;
- with the database in the container, writes a `pg_dump` of it to
  `/var/backups/hmailserver-lxc/hmailserver-<old version>-<time>.dump` first
  (`pg_restore` puts it back);
- stops the service, installs the package with `apt-get` - whose post-install step
  runs `hmailserver --upgrade-database` - and starts it, and asks port 25 (its
  owner, its greeting and STARTTLS) and the REST API again: the status must answer
  401 without the password, which the update does not have. The REST API is asked
  where the server has it then - `RestApiPort` and `RestApiBindAddress` in
  `/etc/hmailserver/hMailServer.ini`, the server's copy of its stored settings (a
  `[SettingsOverride]` line there first) - so a port moved with `hmailserver
  --set-setting` since the install is followed, and a port of 0 skips the question.
  No password is sent, so the certificate is not checked, and one put in the
  install's place does not fail the update. A failure after the package was
  installed says so: that the server was upgraded, from what to what, and whether
  `hmailserver.service` is active.

A snapshot of the container (`pct snapshot <ID> before-update`) before an update is
the quickest way back from one that goes wrong.

The server's own live update ([UpdateFeed.md](UpdateFeed.md)) applies nothing by
itself unless `UpdateAutoDownload` is turned on, which the install does not do; it
installs the generic `.deb`, which is built on Ubuntu 26.04, not the Debian 13
package these scripts install, so in such a container use `update` instead.

What has run
------------

No Proxmox VE host was available, so neither script has run on one: the container's
creation is shown by a dry run and by stand-ins of Proxmox VE's commands, and the
install by Proxmox VE's own template run as a container. On 25 October 2026, on a
Linux build machine of the project's (Docker 29):

- **The container, from Proxmox VE's own template:** `debian-13-standard_13.6-1_amd64.tar.zst`,
  the newest Debian 13 template in Proxmox VE 9's index (`aplinfo-pve-9.dat`, whose
  signature `gpgv` verified as the Proxmox Trixie Release Key's, `24B3 0F06 ECC1 836A
  4E5E FECB A7BC D142 0BFE 778E`), its SHA-512 the one that index gives, imported as
  it is and started with systemd as PID 1, not privileged - given
  `CAP_SYS_ADMIN`, which systemd needs for the units' mount namespaces, and its own
  writable cgroup - with `systemd-binfmt` masked. As Proxmox VE builds it, the
  template has Postfix running on `127.0.0.1:25` and `[::1]:25`, with no STARTTLS.
- **The install,** `hmailserver-lxc install` in that container with the newest
  release, 6.3.7, and the database local: both files verified as signed for `v6.3.7`
  with the project's id, the package held to the signed sums, Postfix stopped and
  removed, the five ports found free, PostgreSQL 17.11 installed, the schema made;
  then port 25 held by the user `hmailserver` alone, `220 hmailserver ESMTP`,
  STARTTLS, and `GET /api/v1/status` 200 at `6.3.7-B46`, with the server listening
  on 25, 110, 143, 587 and 8443 and no bind failure in its log. With an IPv6 address
  added to what `hostname -I` lists, the Control Deck's address was printed as
  `https://[2001:db8::25]:8443/`. `/root/.sigstore` then held the trust root cosign
  had fetched from `tuf-repo-cdn.sigstore.dev`.
- **The update:** 6.3.6 installed the same way in a fresh such container, then
  `ct/hmailserver.sh` run inside it took the update path: 6.3.7 verified, the
  database dumped (844 KiB), the package upgraded (its post-install step left the
  schema at 6108, the version 6.3.7 needs), and the server answered again, port 25
  its own. A second `update`, and `update` with `var_hm_version=6.3.6`, each said
  there was nothing to do and left the server running. In another, the REST API was
  moved to port 9443 with `hmailserver --set-setting` after the install and given a
  certificate of another name: the update followed it there, and the status answered
  401. In a third, with another program listening on `[::1]:25` beside the server,
  the update upgraded the package and then failed at port 25's owner, saying that the
  server had been upgraded from 6.3.6 to 6.3.7 and that `hmailserver.service` was
  active.
- **What the install refuses,** each in a fresh container of the template: a
  listener of another program on port 143 (refused before the package was
  installed, naming the program and its unit); a message in Postfix's queue (refused,
  Postfix left installed and running); nullmailer installed in Postfix's place with a
  message in its queue (refused by name, nothing removed, the message still queued);
  a second `hmailserver-lxc install`. With an external database that is not there,
  the install failed at `--create-database`, after the administrator's password had
  been set, and printed the password with the failure, once; `hmailserver
  --set-admin-password` then set another. And `ask_server`, the install's last step,
  run against a stand-in on port 25 that greets as `220 hmailserver ESMTP` and offers
  STARTTLS, refused it as not the server's, and told a stand-in beside the server's
  own socket from one that kept the server off the port.
- **Exim and starting again:** with Exim installed in Postfix's place, the install
  with `var_rest_port=0` stopped and removed it, saying it had held port 25, passed,
  and said that the REST API and the Deck were off. That server, and the one whose
  install had failed at `--create-database`, were then removed as [When the install
  stops](#when-the-install-stops) says, and the install run again in each passed.
- **The host's script, end to end, against stand-ins** of `pveversion`, `pvesh`,
  `pvesm`, `pveam` and `pct` that answer in Proxmox VE 9's form, `pct exec` and `pct
  push` acting on a container of the template, with `var_db=external` pointing at a
  PostgreSQL 18 server in a container of its own: the stand-ins' log shows `pveam
  update` and `pveam download` only after the summary, then the container made with
  the arguments below, the install script pushed and run, 6.3.7 installed and
  answering on 25 and on `var_rest_port=9443`, the schema made in the external
  database, `update` left in `/usr/bin`. The database password was in no line of the
  output, no file under `/root`, `/tmp`, `/var/tmp`, `/usr/local` or `/var/log` and
  not in the journal (the server's `/etc/hmailserver/hMailServer.ini` holds it, as
  it must).
- **The dry runs:** `var_dry_run=yes`, `bash ct/hmailserver.sh --dry-run`, `bash -c
  "$(cat ct/hmailserver.sh)" _ --dry-run` and `bash -c "$(cat ct/hmailserver.sh)"
  --dry-run`, with the stand-ins: no command that changes the host run, and the
  summary saying it is a dry run. Answered `n` at a terminal, the script changed
  nothing, `pveam update` included, and run with no terminal and without
  `var_yes=yes` it stopped at the question's place. `bash -c "$(cat
  ct/hmailserver.sh)" -h` printed the help, and `-n` in its place was refused. Inside a container, with
  a stand-in of the install script, `update --dry-run` printed the update's two
  commands and ran neither. With no stand-ins, the dry run prints:

  ```text
  + pveam update
  + pveam download local debian-13-standard_13.1-2_amd64.tar.zst
  + pct create 100 local:vztmpl/debian-13-standard_13.1-2_amd64.tar.zst --hostname hmailserver --cores 2 --memory 2048 --swap 512 --rootfs local-lvm:16 --net0 name=eth0\,bridge=vmbr0\,ip=dhcp --unprivileged 1 --ostype debian --onboot 1 --tags hmailserver\;mail --description Progressive\ Robot\'s\ fork\ of\ hMailServer\ \(https://gitlab.com/hmailserver/hmailserver\)\,\ made\ by\ its\ ct/hmailserver.sh --features nesting=1
  + pct start 100
  + pct push 100 /tmp/tmp.XXXXXXXXXX/hmailserver-install.sh /usr/local/sbin/hmailserver-lxc --perms 0755
  + pct push 100 /tmp/tmp.XXXXXXXXXX/install.env /root/.hmailserver-lxc.env --perms 0600
  + pct exec 100 -- bash /usr/local/sbin/hmailserver-lxc install --env-file /root/.hmailserver-lxc.env
  + pct push 100 /tmp/tmp.XXXXXXXXXX/update /usr/bin/update --perms 0755
  ```

  (off a Proxmox VE host the ID and the template's name are made up).
- **The host's refusals:** an ID the host has, a `var_net` holding a comma, a
  privileged container, Proxmox VE 7, an arm64 host, an unknown argument, the script
  run without `--dry-run` on a machine that is not a Proxmox VE host; and, of the
  download's checks, each run against a file made to fail it: the package with one
  byte changed, the package checked as signed for another tag, 6.3.5's package
  (signed at the project's first path) checked at the group's, 6.3.3's bundle (signed
  by GitHub Actions, another repository id), sums with an altered line, without the
  package's line and naming it twice, and altered sums against the sums' bundle. Each
  was refused, and the real files passed.
- **shellcheck** 0.10.0 passes both scripts.

What it does not show: Proxmox VE's own `pct create` and `pct start` with these
arguments, `pveam`'s download of the template, an unprivileged container's user
namespace and AppArmor profile (the container above has neither), `var_nesting=0`,
a mail server other than Postfix, Exim and nullmailer (the others are refused by
name, as nullmailer is), starting again with an external database, an arm64
machine, and `update` fetching the scripts from `master`, which holds them only once
this change has landed.
