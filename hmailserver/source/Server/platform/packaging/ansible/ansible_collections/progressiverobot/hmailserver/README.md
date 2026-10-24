# progressiverobot.hmailserver

An Ansible collection that installs and configures **Progressive Robot's fork of
hMailServer** on Linux. This is not the original hMailServer, which is Martin
Knafve's and is developed separately; the fork has its own releases, from 6.0.0, its
own Linux packages and a REST API the original does not have. The project's account
of the two is
[ForkAndOriginal.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/ForkAndOriginal.md).

The collection has one role, `progressiverobot.hmailserver.hmailserver`. It installs
the server from the fork's own channel for the distribution, points it at a PostgreSQL
or MariaDB database you name, sets the administrator password, creates or upgrades the
database's schema, makes the installation's stored-secrets key, stores the settings
you give it, and starts and enables the service. It opens no firewall port and
installs no database server.

## Where it installs from, and what it verifies

| Distribution | Channel | What is verified |
|---|---|---|
| Ubuntu 24.04 and 26.04 | the PPA `ppa:hmailserver/hmailserver` | apt checks the packages against the PPA's key, which the role fetches from Ubuntu's key server and imports only when it is the key `F1701CED8D88F87E19B08F7B0ADD4C9C6D462C8C` |
| Debian 12 and 13 | the release's `.deb`, from the project's package registry on GitLab | the file is installed only when its SHA-256 is the one pinned for it: this collection pins the `.deb` files of the release it is built for, taken from the release's `hmailserver-linux-<version>-SHA256SUMS.txt` after its Sigstore bundle verified |
| Fedora, and Enterprise Linux 10 | Copr `progressiverobot/hmailserver` | dnf checks the packages against the project's Copr key, imported only when it is `A43209E902FC9666B7DC7C7007AD13D308C7F2E7` |
| openSUSE Leap 16.0 (x86_64 and aarch64) and Tumbleweed (x86_64) | the Build Service project `home:progressiverobot` | zypper checks the repository's metadata and the packages against the project's key there, imported only when it is `BA0868309CFBD36498CE41D22F0B9419E67EE770` |

A distribution the project publishes no package for is refused before anything is
changed.

## Using it

```yaml
- name: A mail server
  hosts: mail
  become: true
  roles:
    - role: progressiverobot.hmailserver.hmailserver
      vars:
        hmailserver_database_type: PostgreSQL
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

Install it with `ansible-galaxy collection install progressiverobot.hmailserver`
once it is published on Ansible Galaxy, or from a checkout of the project with
`ANSIBLE_COLLECTIONS_PATH` naming
`hmailserver/source/Server/platform/packaging/ansible`. It needs
`community.general` (its `ini_file` and `zypper` modules) and ansible-core 2.19 or
later.

The database user must exist. With `hmailserver_database_create` (the default) the
role runs `hmailserver --create-database`, which creates the database where the user
may, fills an empty one, and leaves one that already holds the schema as it is; an
older schema is brought forward with `--upgrade-database`. The first start after a
new database secures it - a self-signed certificate, STARTTLS on 25, 587, 110 and 143,
and a password refused without TLS from any address but the machine's own
([SecureDefaults.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/SecureDefaults.md)).

Every variable, with its default, is in `roles/hmailserver/defaults/main.yml`, and
`roles/hmailserver/meta/argument_specs.yml` says what each takes; Ansible holds them to
it when the role starts. The two passwords are required and are never logged; the
administrator password is written as a PBKDF2 hash by the server's own
`--set-admin-password`, by default only when the configuration has none
(`hmailserver_admin_password_update: always` sets it on every run). Domains and
accounts are made over the REST API when the server does not have them, and are
never changed or removed.

Back up the stored-secrets key with the database: a database restored without it has
lost every second factor and stored secret
([SecretsAtRest.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/SecretsAtRest.md)).

## How it is tested

Molecule scenarios under `extensions/molecule` run the role in containers with
systemd as PID 1, each beside a database server of its own: Ubuntu 26.04 and 24.04,
Debian 12 and 13, Debian 13 against MariaDB, Fedora 44, AlmaLinux 10, openSUSE Leap
16.0 and Tumbleweed. Each converges, converges again with nothing changed, and is
then asked for the version installed, the service enabled and running as
`hmailserver`, SMTP on 25 with STARTTLS taken, `GET /api/v1/status` with the
administrator password and a 401 with a wrong one, the domain and account it made,
the configuration's owner and mode, and the exported key. The project's documentation
says how to run them and what they showed
([Ansible.md](https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/Ansible.md)).
`ansible-lint` (production profile) and `ansible-test sanity` run over the collection
in the project's CI.

## Licence

AGPL-3.0-or-later, as the server is. Issues go to
<https://gitlab.com/hmailserver/hmailserver/-/issues>.
