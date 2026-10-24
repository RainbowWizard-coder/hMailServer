Ansible: the collection progressiverobot.hmailserver
====================================================

**An Ansible collection in the tree installs and configures this project - Progressive
Robot's fork of hMailServer, not the original hMailServer - on Linux from the
project's own packages: the Ubuntu PPA, the release's `.deb` for Debian, the Fedora
Copr for Fedora and Enterprise Linux 10, and the openSUSE Build Service project. Its
role points the server at a PostgreSQL or MariaDB database, sets the administrator
password, creates or upgrades the schema, makes the stored-secrets key and starts the
service. It is not on Ansible Galaxy yet: the jobs that build and publish it are
written and wait for the owner's Galaxy account ([Publishing to
Galaxy](#publishing-to-galaxy)).**

| | |
|---|---|
| The collection | `hmailserver/source/Server/platform/packaging/ansible/ansible_collections/progressiverobot/hmailserver` |
| Its role | `progressiverobot.hmailserver.hmailserver` |
| Its version | the release's whose `.deb` files it pins: 6.3.7 in the tree |
| Needs | ansible-core 2.19 or later and `community.general`, on the controller; Python 3 and systemd on the server |
| CI | `galaxy-check`, `galaxy-build`, `galaxy-publish` (`.gitlab/ci/channels.yml`, `build/ci/channels/galaxy-publish.py`) |

Why it is where it is: beside the other packaging, under
`packaging/ansible/ansible_collections/<namespace>/<name>`, the layout `ansible-test`
requires of a collection it tests and the one `ANSIBLE_COLLECTIONS_PATH` finds, so the
tree's copy is used, tested and built in place. Its deepest paths are 156 characters
long, the longest in the tree, so that a Windows checkout in a folder whose own path
is longer than about 100 characters needs `git config core.longpaths true`.

Installing with it
------------------

From a checkout of the project, before it is on Galaxy, with `community.general`
installed outside the checkout:

    export ANSIBLE_COLLECTIONS_PATH="$PWD/hmailserver/source/Server/platform/packaging/ansible:$HOME/.ansible/collections"
    ansible-galaxy collection install -p "$HOME/.ansible/collections" 'community.general:>=9.0.0'

and once it is published:

    ansible-galaxy collection install progressiverobot.hmailserver

A playbook, run with `become: true`:

```yaml
- name: A mail server
  hosts: mail
  become: true
  roles:
    - role: progressiverobot.hmailserver.hmailserver
      vars:
        hmailserver_database_type: PostgreSQL          # or MySQL, for MySQL and MariaDB
        hmailserver_database_host: db.example.com
        hmailserver_database_password: "{{ vault_hmailserver_database_password }}"
        hmailserver_admin_password: "{{ vault_hmailserver_admin_password }}"
        hmailserver_stored_secrets_key_export: /root/hmailserver-stored-secrets.key
        hmailserver_settings:
          RestApiPort: 8080
        hmailserver_domains:
          - name: example.com
            postmaster: postmaster@example.com
        hmailserver_accounts:
          - address: ann@example.com
            password: "{{ vault_ann_password }}"
```

The database server is yours: the role installs none, and its user must exist. With
`--create-database` the user creates the database where it may (PostgreSQL's
`CREATEDB`), and otherwise an empty database made for it is filled
(platform/packaging/README.md, "Install and get running", step 2, has the commands).

What the role does, in order - the packaging guide's steps, with the server's own
commands:

1. **The package**, from the channel for the distribution (below). On MySQL or
   MariaDB, also the client library the server opens with `dlopen`, which no package
   depends on (`libmariadb3`, `mariadb-connector-c`).
2. **`[Database]`** of `/etc/hmailserver/hMailServer.ini`: `Type`, `Server`, `Port`
   (5432 or 3306 when it is not given), `Database`, `Username`, `Password` and
   `Passwordencryption=0`, the file kept `0640 root:hmailserver`. The password is
   never logged.
3. **The administrator password**, with `hmailserver --set-admin-password` on
   standard input, written as a PBKDF2 hash and never logged: when the configuration
   has none (`hmailserver_admin_password_update: on_create`), or on every run
   (`always`).
4. **`--check-config`** as the service's user. It says what it read and exits 0
   whatever that was, so the role reads what it says, and stops when the database
   type is not the one it wrote or there is no data directory: a file the service's
   user cannot read, or a `Type` the server does not take.
5. **`--create-database`** as the service's user (`hmailserver_database_create`),
   which leaves a database that holds the schema as it is, and **`--upgrade-database`**
   when the schema is older (`hmailserver_database_upgrade`).
6. **The stored-secrets key**, `--create-stored-secrets-key` as root
   (`hmailserver_stored_secrets_key`), exported once to
   `hmailserver_stored_secrets_key_export` when that names a path
   ([SecretsAtRest.md](SecretsAtRest.md)). Keep the export with the database backups.
7. **`[Settings]`** values from `hmailserver_settings`, each with `--set-setting
   NAME` and its value on standard input, never on a command line, where the process
   list would show it; the task is not logged, and a setting the server refuses is
   named with the server's reason, which never repeats a value. A secret setting is
   taken the same way; since the server never shows a secret it holds, one is
   reported changed, and the service restarted, on every run.
8. **The service**, enabled and started; a service that ran before the role changed
   its configuration is restarted. The first start after a new database secures it
   ([SecureDefaults.md](SecureDefaults.md)). The role waits for the SMTP banner on
   `127.0.0.1:25` (`hmailserver_wait_for_smtp` seconds).
9. **Domains and accounts** over the REST API, when `hmailserver_domains` or
   `hmailserver_accounts` name any: each made when the server does not have it, and
   never changed or removed. It needs `RestApiPort`; a 429 the API answers after
   refused sign-ins is waited out.

The role opens no firewall port and changes no firewall. Every variable, with its
default, is in `roles/hmailserver/defaults/main.yml`; `meta/argument_specs.yml` says
what each takes, and Ansible holds them to it when the role starts.

What it verifies
----------------

| Distribution | Channel | What is checked |
|---|---|---|
| Ubuntu 24.04 (`noble`) and 26.04 (`resolute`), amd64 and arm64 | `ppa:hmailserver/hmailserver`, as a deb822 source signed by the PPA's key alone | the key is fetched from Ubuntu's key server by its fingerprint and put in `/etc/apt/keyrings/hmailserver-ppa.asc` only when `gpg --show-keys` reads it as the one key `F1701CED8D88F87E19B08F7B0ADD4C9C6D462C8C` (Launchpad's `signing_key_fingerprint` for the PPA); apt then checks the archive's signature against it |
| Debian 12 and 13 | the release's `hmailserver_<v>-1~debian<n>_<arch>.deb`, from the project's generic package registry, as the release links it | `get_url` holds the download to the SHA-256 pinned for it, and nothing else is installed: `vars/release.yml` pins the four `.deb` files of the release the collection is built for, as its `hmailserver-linux-<v>-SHA256SUMS.txt` lists them after that file verified against its Sigstore bundle as this project's pipeline for the tag. Another version needs its SHA-256 in `hmailserver_deb_sha256`, taken from that release's sums file verified the same way, or the role refuses it |
| Fedora 43 and 44 (`fedora-<n>`), Enterprise Linux 10 (`epel-10`), x86_64 and aarch64 | Copr `progressiverobot/hmailserver` | the key Copr serves for the project is fetched to a temporary file and imported only when `gpg --show-keys` reads it as the one key `A43209E902FC9666B7DC7C7007AD13D308C7F2E7`; the repository is `gpgcheck=1`, with no `gpgkey` through which dnf could import another |
| openSUSE Leap 16.0 (x86_64 and aarch64) and Tumbleweed (x86_64) | the Build Service's `home:progressiverobot` | the same, for the key `BA0868309CFBD36498CE41D22F0B9419E67EE770`; the repository is `gpgcheck=1` and `repo_gpgcheck=1`, with no `gpgkey` |

The fingerprints were read on 24 October 2026 from Launchpad's API and from the keys
Copr and the Build Service publish for the project. Copr's key expires in 2031 and the
Build Service's in 2028, and each service extends its own: the role fetches the key
from the service each time it imports one, so an extended key is taken, and a file
holding a key with another fingerprint, or another key beside the right one, is
refused with nothing imported. `ansible.builtin.rpm_key` alone would not do: given a
fingerprint, it imports every key of a file in which that fingerprint is one among
several. A release or architecture a channel does not build for (`vars/main.yml`
names what each builds) is refused before anything is changed.
`galaxy-publish.py check` holds each fingerprint to 40 hex digits, the PPA's key to
being fetched by the fingerprint it is held to, and the role's checks to being there:
each key held to its fingerprint and read as one key alone, the repositories'
`gpgcheck` and the Build Service's `repo_gpgcheck` on and no `gpgkey`, the `.deb`
held to its SHA-256, and the tasks that carry a password or a setting not logged.

How it is tested
----------------

**The molecule scenarios** are under the collection's `extensions/molecule`: `ubuntu2604`,
`ubuntu2404`, `debian13`, `debian12`, `debian13-mariadb`, `fedora44`, `almalinux10`,
`leap16` and `tumbleweed`. Each makes a Docker network of its own, a database server
on it (PostgreSQL 18, or MariaDB 11.8 for `debian13-mariadb`, each by digest) and the
distribution's own image, by digest, with systemd added as PID 1; then converges the
role, converges it again and fails if anything changed, and asks the running server
for:

- `hmailserver --version`, the version the package manager installed: on Debian the
  release the collection is built for, and from a channel that release or a later
  one;
- the unit `active`, `running`, `enabled`, as the user `hmailserver`;
- SMTP on `127.0.0.1:25`: the 220 banner, EHLO offering STARTTLS, and STARTTLS taken
  (`utils/smtp_check.py`) - which the first start's new-installation defaults turn on;
- `GET /api/v1/status` answering 200 with the administrator password and 401 with a
  wrong one;
- the domain and the account the role made, over the API;
- `hMailServer.ini` `0640 root:hmailserver`, its administrator password set and not
  as it was given;
- the exported stored-secrets key, `0600`.

The passwords are made by Ansible's password lookup in the scenario's own ephemeral
directory at its first use, and are never in the tree.

The platform's container is not privileged. It has `CAP_SYS_ADMIN`, which systemd
needs to build the server unit's mount namespace ([PackagesDeb.md](PackagesDeb.md),
"Started under systemd", says why), AppArmor unconfined so that its command may
remount its own cgroup - its own subtree, in a cgroup namespace of its own - writable
(a bind remount, which changes no other mount) before systemd starts, and `/proc/sys`
and `/sys` read-only as in any unprivileged container. `systemd-binfmt` and the
`binfmt_misc` mount are masked in the image, so that it never touches the host's
handlers. Every name begins with `HM_MOLECULE_PREFIX` (`hmailserver-molecule` when it
is not set), and the scenario's destroy step removes its containers, its image, its
database's volume and its network.

To run them, with the tools of `build/ci/ansible-collection-requirements.txt` and a
Docker the controller reaches:

    python3 -m venv /tmp/ansible
    /tmp/ansible/bin/pip install --require-hashes -r build/ci/ansible-collection-requirements.txt
    export PATH=/tmp/ansible/bin:$PATH
    cd hmailserver/source/Server/platform/packaging/ansible/ansible_collections/progressiverobot/hmailserver
    molecule test -s debian13            # or --all

**In CI**, `galaxy-check` runs in a merge request's or a branch's pipeline when the
collection, its script or what checks it changes, never for a tag: the tools from
the same hashed requirements file; `galaxy-publish.py check` and its self-test, which
builds the collection with the real `ansible-galaxy` too; the collection built and
kept as the job's artifact; `ansible-lint --offline --profile production`; and
`ansible-test sanity` but for four of its tests. `pep8`, `pylint`, `runtime-metadata`
and `yamllint` are the ones for which `ansible-test` installs tools into virtual
environments of its own, from PyPI at versions it pins but without their hashes,
which no job of the project's does; the collection's YAML and `meta/runtime.yml` are
`ansible-lint`'s too, with yamllint and the schemas from the hashed file, and a
sanity run that fetches anything fails the job. All 24 are run by hand, as are the
molecule scenarios, which need a Docker of their own ([What has run](#what-has-run)).

Publishing to Galaxy
--------------------

Two jobs in a `master` pipeline, each pressed by hand, as every channel's are
([Channels.md](Channels.md), "How a channel job publishes"):

- **`galaxy-build`** takes the release by the channels' rule (`HM_CHANNEL_VERSION`,
  or the update feed's latest, published), verifies its
  `hmailserver-linux-<version>-SHA256SUMS.txt` against its Sigstore bundle as this
  project's pipeline for the tag, reads the four Debian `.deb` pins from it, and
  builds the collection from `master`'s tree with that version in `galaxy.yml` and
  those pins in `vars/release.yml`. The tarball is read back: its `MANIFEST.json` must
  give the namespace, name, version and licence, its `release.yml` the version and
  pins it was built with, and it must carry the repository's `LICENSE`. It publishes
  nothing, and is the dry run before the namespace exists.
- **`galaxy-publish`** follows it, with its artifacts: without `GALAXY_API_KEY` it
  says "not configured" and passes. With it, it holds the tarball to the SHA-256
  `galaxy-build` recorded and to the release the job takes now, asks Galaxy whether
  it has the version (a version is kept for good and refused a second time, so one
  Galaxy has is not sent again), and sends it with `ansible-galaxy collection
  publish`, which waits for Galaxy's import. The token reaches `ansible-galaxy` in its
  environment, as the server list `release_galaxy`'s token, never on a command line,
  and is cut out of anything it prints.

So the collection on Galaxy is always a release's version, built from `master`'s
role: a change to the role reaches Galaxy with the next release.

**The owner, once.** Sign in to galaxy.ansible.com with the GitHub account
`RainbowWizard-coder` (Galaxy's only sign-in). Galaxy makes a namespace of the
login's own name at the first sign-in; the namespace `progressiverobot` is another,
and is requested on the Ansible forum with its Galaxy namespace request form
(forum.ansible.com, "Ansible Galaxy: how to request a custom namespace"). Once the
namespace is the account's, make an API token (Galaxy's "API token" page); a maintainer stores it
as the protected, masked project variable `GALAXY_API_KEY`, scoped to the environment
`galaxy`. Then press `galaxy-build`, and `galaxy-publish` after it, in a `master`
pipeline.

What has run
------------

On 24 October 2026, over the collection as it is in the tree, on a Linux machine with
Docker 29.1.3, with the tools of `build/ci/ansible-collection-requirements.txt`
installed with their hashes: Python 3.14.7, ansible 14.5.0 (ansible-core 2.21.5,
community.general 13.5.0, community.docker 5.4.0), ansible-lint 26.10.1 and molecule
26.10.0.

- **The pins.** `release-files.py verify` (cosign 2.6.5 out of the pinned
  `HM_COSIGN_IMAGE`) verified `hmailserver-linux-6.3.7-SHA256SUMS.txt` as
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.7`, the
  repository id 86730559 in its certificate; `vars/release.yml`'s four SHA-256 are its
  lines for the Debian 12 and 13 `.deb` files.
- **Every scenario passed `molecule test`**: `ubuntu2604` (159 s), `ubuntu2404` (92 s),
  `debian13` (74 s), `debian12` (69 s), `debian13-mariadb` (83 s), `fedora44` (82 s),
  `almalinux10` (81 s), `leap16` (400 s, most of it building the image) and
  `tumbleweed` (107 s). Each converge changed 13 to 17 things, the second converge
  nothing, and each verify passed: the version 6.3.7, the unit running as
  `hmailserver`, SMTP's banner, EHLO and STARTTLS taken (TLS 1.3 on all nine), the
  status with the password and 401 without it, the domain and the account, the
  configuration's mode and the key's. The host's `binfmt_misc` handlers were the same
  before and after each.
- **With ansible-core 2.19.14** (ansible 12.3.0, installed with the hashes PyPI gives
  for them in place of the file's two), the oldest `meta/runtime.yml` names, `debian13`
  (73 s) and `ubuntu2604` (85 s) passed `molecule test` too, under Python 3.14, which
  2.19 does not list among its controller's Pythons (it warns, and ran).
- **The refusals**, each shown to happen by a converge with one value broken, as an
  extra variable, or with the machine made so first, which then failed with the role's
  words and left what it guards as it was:
  - the keys: Copr's and the Build Service's key served as a file that also holds a
    throwaway key, in one armour block and as two (given to the role as a `file://`
    address standing for the channel's), and their fingerprints one digit off - each
    "is not the channel's key alone", with no key of either imported, no repository
    written and no package installed; the PPA's fingerprint one digit off, with no key
    file and no source written;
  - what the channels build: Copr's chroots without the machine's (`fedora-44`) and
    its architectures without `x86_64`, the PPA's and the Build Service's
    architectures without it - each refused before any key, repository or source;
    the `.deb` channel asked for on Ubuntu;
  - the `.deb`: a pinned SHA-256 that is not the file's (`get_url`: "did not match"),
    and a version with no pin (`hmailserver_version=6.3.6`);
  - the configuration: `/etc/hmailserver` made `0700`, so that the service's user
    cannot read it - `--check-config` exited 0 and read database type 0 and no data
    directory, and the role stopped; with the directory as it was, the next converge
    passed;
  - a setting the server does not read (`NoSuchSettingHere`), refused with the
    server's own reason and nothing of any value in the output; and an empty
    administrator password.
- **The installed package.** With a `.deb` of 6.3.7 installed, a collection built for
  an older release (`hmailserver_release_version=6.3.6`, with its pin) left it as it
  was, its download and install skipped, under `present` and under `latest`; asking
  for 6.3.6 by `hmailserver_version` was refused by apt as a downgrade, and 6.3.7 stayed.
- **Nothing logged.** `debian13` converges at `-v`, one of them with a secret setting
  (`SRSSecret`, a throwaway marker as its value) beside `RestApiPort`, held neither the
  marker nor any of the scenario's three passwords, read back from its directory, in
  any of their logs; the setting was stored, and the configuration holds it sealed,
  not as given.
- **The linters.** `ansible-lint --offline --profile production`: 0 failures, 0
  warnings over 35 files. The 20 tests of `ansible-test sanity --python 3.14` the
  `galaxy-check` job runs, in a git checkout of the tree and a fresh home: passed,
  with nothing fetched. All 24, with `ansible-test`'s own pinned tools for the four
  that install them, passed over the collection's first version, whose Python and
  `meta/runtime.yml` are as they are now.
- **The CI scripts.** `galaxy-publish.py check` and `--self-test` (every case, the
  real `ansible-galaxy` build among them), `build/check-channels.py` and its self-test;
  and `galaxy-publish.py`'s publish, run with the real `ansible-galaxy` against a Galaxy
  of the test's own on the loopback: it asked for the version (404), sent
  `POST /api/v3/artifacts/collections/` with the tarball and `Authorization: Token`
  and the key, waited for the import, found the version, and a second run sent nothing.
