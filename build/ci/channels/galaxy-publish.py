#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Ansible collection progressiverobot.hmailserver, checked, built for a release and
published to Ansible Galaxy (.gitlab/ci/channels.yml's galaxy-check, galaxy-build and
galaxy-publish; hmailserver/docs/Ansible.md).

The collection is in the tree at
hmailserver/source/Server/platform/packaging/ansible/ansible_collections/progressiverobot/
hmailserver. Its role installs the server from the project's channels; on Debian it
installs the release's .deb, held to the SHA-256 pinned in roles/hmailserver/vars/
release.yml for the release whose version galaxy.yml carries. So a collection is built
for one release, and both are written together:

  check          the tree's collection holds together, with nothing fetched:
                 galaxy.yml's namespace, name, licence and addresses, its description
                 saying it is Progressive Robot's fork and not the original, its
                 version the one vars/release.yml pins the .deb files of, each pin
                 64 hex digits for each Debian release and architecture the role
                 takes, the channels' key fingerprints 40 hex digits, a heading in
                 its CHANGELOG.md for its version, the role's checks of what it
                 installs and of what it logs there and the keywords they carry
                 (VERIFIED, FORBIDDEN), what its package, repository and download
                 modules are given (MODULE_ARGS), the names the checks read set once
                 (CHECKED_NAMES, PINNED_NAMES), the PPA's apt source as written
                 (PPA_SOURCE), no default for either password, and no address on
                 GitHub anywhere in it;
  build          the collection staged from the tree with the repository's LICENSE,
                 version and pins rendered when they are given, built by
                 ansible-galaxy collection build, and the tarball read back: its
                 MANIFEST.json the namespace, name, version and licence, its
                 release.yml the version and pins it was built with, LICENSE in it;
  release-build  the galaxy-build job: the release, as every channel takes it
                 (build/ci/channels/release-files.py: HM_CHANNEL_VERSION or the feed's
                 latest, published), refused before anything is fetched unless the
                 collection's CHANGELOG.md has a "## <version>" heading for it; its
                 hmailserver-linux-<version>-SHA256SUMS.txt
                 verified against its Sigstore bundle as this project's pipeline for
                 the tag, the .deb pins read from it, and the collection built for that
                 release from this checkout - master's, as chocolatey-publish renders
                 the latest release's package from master's templates - into
                 galaxy-build/, with galaxy-build.json saying what was built from what.
                 Nothing is published;
  publish        the galaxy-publish job: dormant without GALAXY_API_KEY (a protected,
                 masked project variable, the API token of the Galaxy account that
                 owns the namespace progressiverobot): it says "not configured" and
                 exits 0. Otherwise the tarball galaxy-build made, held to the SHA-256
                 it recorded and to the release the job takes now, is sent with
                 ansible-galaxy collection publish, which waits for Galaxy's import;
                 the token reaches it in its environment
                 (ANSIBLE_GALAXY_SERVER_RELEASE_GALAXY_TOKEN), never on a command line.
                 A version Galaxy already has is not sent again: Galaxy keeps a
                 version for good, and refuses it a second time.

Usage, from the root of a checkout:
  python3 build/ci/channels/galaxy-publish.py check
  python3 build/ci/channels/galaxy-publish.py build --out DIR
  python3 build/ci/channels/galaxy-publish.py release-build --out galaxy-build
  python3 build/ci/channels/galaxy-publish.py publish --from galaxy-build
  python3 build/ci/channels/galaxy-publish.py --self-test
"""

import argparse
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def load(name, path):
    """A module of this repository by its path, once per process: every channel script
    shares one release-files.py, and so one Refused, whichever loads it first."""
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


rf = load("release_files", os.path.join(HERE, "release-files.py"))
Refused = rf.Refused

COLLECTION = os.path.join("hmailserver", "source", "Server", "platform", "packaging", "ansible", "ansible_collections",
                          "progressiverobot", "hmailserver")
GALAXY_YML = "galaxy.yml"
RELEASE_YML = os.path.join("roles", "hmailserver", "vars", "release.yml")
MAIN_VARS = os.path.join("roles", "hmailserver", "vars", "main.yml")
DEFAULTS = os.path.join("roles", "hmailserver", "defaults", "main.yml")
ROLE_META = os.path.join("roles", "hmailserver", "meta", "main.yml")
CHANGELOG = "CHANGELOG.md"
TASKS = os.path.join("roles", "hmailserver", "tasks")
NAMESPACE = "progressiverobot"
NAME = "hmailserver"
LICENCE = "AGPL-3.0-or-later"
REPOSITORY = "https://gitlab.com/hmailserver/hmailserver"
ISSUES = REPOSITORY + "/-/issues"
# The Debian releases and architectures the release carries a .deb for, and the role
# installs on (vars/main.yml, hmailserver_deb_releases and hmailserver_deb_architectures).
TARGETS = ("debian12_amd64", "debian12_arm64", "debian13_amd64", "debian13_arm64")
FORK = "Progressive Robot's fork of hMailServer"
NOT_ORIGINAL = "not the original hMailServer"
GALAXY = "https://galaxy.ansible.com"
GALAXY_API = GALAXY + "/api/"
SERVER_ID = "release_galaxy"
SUMS = "hmailserver-linux-%s-SHA256SUMS.txt"
RECORD = "galaxy-build.json"
SKIP = (".ansible", "__pycache__", "output")
# What follows a YAML sequence's item while the sequence goes on - blank lines, comments
# and lines indented deeper - and then a further item: the second licence of a license
# list given one more.
FURTHER_ITEM = r"(?=(?:\n(?:[ \t]*(?:#.*)?|[ \t]+.*))*\n[ \t]*-(?:[ \t]|$))"
# The role's checks of what it installs and of what it logs, each a task of a file of
# its tasks/ by name with the lines it must hold (Ansible.md, "What it verifies"): with
# one of them gone or changed the role would install what it was not given to, or log
# a secret, and nothing it runs would show it. Each entry is (file, task, lines, what,
# control, keys). lines are held whole (held()): each text as written, with its
# indentation, and the line after it indented no deeper than its last line's, or than
# the number given beside it, so that nothing continues its last line - a plain or
# folded scalar goes on over the lines indented deeper than its item or key, and
# "- (a) == 1" followed by a line "or true" is a condition that always holds. control
# is None for a task held for what it does not log; for a check, the task keywords of
# CONTROL_KEYS it may carry, each once, which would otherwise skip it or take its
# failure for a success. keys are the other task keywords it may carry beside name,
# each once (task_keys()): any other - tags, which with never skip it, loop or a
# with_ lookup, which over an empty list skip it, until, check_mode, delegate_to,
# environment, args - is refused, and so is a line there that is no keyword. The
# blocks round the checks are held so too, since a block's keywords are its tasks'.
# An assertion is held as its whole that: list, so that a condition cannot be made
# inert or moved out of it.
CONTROL_KEYS = ("when", "failed_when", "ignore_errors", "ignore_unreachable")
RPM_WORK = "{{ hmailserver_rpm_key_work.path }}"
# How the settings are stored: the name on the command line, the value on standard
# input. Nothing else of that task may name a setting's value but its changed_when
# (SETTING_VALUE), nor hmailserver_settings but its loop (SETTINGS_LOOP), nor a variable
# by its name as a string (vars[...], hostvars[...]).
SETTINGS_TASK = ("configure.yml", "Store the settings")
SETTINGS_COMMAND = ("  ansible.builtin.command:\n    argv:\n      - hmailserver\n      - --set-setting\n"
                    "      - \"{{ item.key if (item.value | string | trim | length) > 0 else item.key ~ '=' }}\"\n"
                    "    stdin: \"{{ item.value | string }}\"", 2)
SETTINGS_LOOP = "  loop: \"{{ hmailserver_settings | dict2items }}\""
# Any use of the loop's item but its key: item.value, item['value'], item itself.
SETTING_VALUE = r"\bitem\b(?!\.key\b)"
COMMAND = "ansible.builtin.command"
ASSERT = "ansible.builtin.assert"
SLURP = "ansible.builtin.slurp"


def rpm_include(url, fingerprint):
    """The include of rpm-key.yml for a channel, held whole: its url and fingerprint the
    only variables it gives, and nothing after them."""
    return (("  ansible.builtin.include_tasks: rpm-key.yml\n  vars:\n    hmailserver_rpm_key_url: \"%s\"\n"
             "    hmailserver_rpm_key_fingerprint: \"{{ %s }}\"" % (url, fingerprint), 2),)


VERIFIED = (
    ("rpm-key.yml", "Fetch, check and import the key", (("  block:", 4),),
     "the RPM channel's key fetched, checked and imported", (), ("block", "always")),
    ("rpm-key.yml", "Fetch the key from the channel",
     ('      ansible.builtin.get_url:\n        url: "{{ hmailserver_rpm_key_url }}"\n'
      '        dest: "%s/fetched.asc"' % RPM_WORK,),
     "an RPM channel's key fetched into the file read", (), ("ansible.builtin.get_url", "changed_when")),
    ("rpm-key.yml", "Read the fetched file", ('      ansible.builtin.slurp:\n        src: "%s/fetched.asc"' % RPM_WORK,),
     "an RPM channel's key file read whole", (), (SLURP, "register")),
    ("rpm-key.yml", "Read the fetched key's fingerprint",
     ('      ansible.builtin.command:\n        argv: [gpg, --batch, --no-autostart, --homedir, "%s", --show-keys,'
      ' --with-colons,\n               "%s/fetched.asc"]' % (RPM_WORK, RPM_WORK),),
     "gpg reading the RPM channel's key file fetched", (), (COMMAND, "register", "changed_when")),
    ("rpm-key.yml", "Refuse a file that is not the channel's key alone",
     (("      ansible.builtin.assert:\n        that:\n"
       "          - (hmailserver_rpm_key_fetched.content | b64decode | regex_findall('-----BEGIN PGP ') | length) == 1\n"
       "          - (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n"
       "          - >-\n"
       "            (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^fpr:+([0-9A-F]{40}):') | first | default(''))\n"
       "            == hmailserver_rpm_key_fingerprint", 8),),
     "an RPM channel's key file read as its one key alone, in one armour block", (), (ASSERT,)),
    ("rpm-key.yml", "Put the key in a keyring of its own",
     ('      ansible.builtin.command:\n        argv: [gpg, --batch, --no-autostart, --homedir, "%s", --import,\n'
      '               "%s/fetched.asc"]' % (RPM_WORK, RPM_WORK),),
     "the RPM channel's key put in a keyring of its own", (), (COMMAND, "changed_when")),
    ("rpm-key.yml", "Export that key alone",
     ('      ansible.builtin.command:\n        argv: [gpg, --batch, --no-autostart, --homedir, "%s", --armor, --output,\n'
      '               "%s/key.asc", --export, "{{ hmailserver_rpm_key_fingerprint }}"]' % (RPM_WORK, RPM_WORK),),
     "the RPM channel's key exported by its fingerprint alone", (), (COMMAND, "changed_when")),
    ("rpm-key.yml", "Read the exported key", ('      ansible.builtin.slurp:\n        src: "%s/key.asc"' % RPM_WORK,),
     "the exported key read whole", (), (SLURP, "register")),
    ("rpm-key.yml", "Read the exported key's fingerprint",
     ('      ansible.builtin.command:\n        argv: [gpg, --batch, --no-autostart, --homedir, "%s", --show-keys,'
      ' --with-colons,\n               "%s/key.asc"]' % (RPM_WORK, RPM_WORK),),
     "gpg reading the exported key", (), (COMMAND, "register", "changed_when")),
    ("rpm-key.yml", "Refuse an export that is not the channel's key alone",
     (("      ansible.builtin.assert:\n        that:\n"
       "          - (hmailserver_rpm_key_exported.content | b64decode | regex_findall('-----BEGIN PGP ') | length) == 1\n"
       "          - (hmailserver_rpm_key_export_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n"
       "          - >-\n"
       "            (hmailserver_rpm_key_export_read.stdout | regex_findall('(?m)^fpr:+([0-9A-F]{40}):') | first"
       " | default(''))\n"
       "            == hmailserver_rpm_key_fingerprint", 8),),
     "the exported key read as that one key alone", (), (ASSERT,)),
    ("rpm-key.yml", "Import the key, held to its fingerprint",
     ('      ansible.builtin.rpm_key:\n        key: "%s/key.asc"\n'
      '        fingerprint: "{{ hmailserver_rpm_key_fingerprint }}"' % RPM_WORK,),
     "an RPM channel's key imported as gpg's export of it, at its fingerprint", (), ("ansible.builtin.rpm_key",)),
    ("install-copr.yml", "Import Copr's key for the project, held to its fingerprint",
     rpm_include("{{ hmailserver_copr_base }}/pubkey.gpg", "hmailserver_copr_fingerprint"),
     "Copr's key held to its fingerprint", (), ("ansible.builtin.include_tasks", "vars")),
    ("install-copr.yml", "Name the Copr repository, its packages checked against that key", ("    gpgcheck: true",),
     "Copr's packages checked against its key", (), ("ansible.builtin.yum_repository",)),
    ("install-obs.yml", "Import the Build Service's key for the project, held to its fingerprint",
     rpm_include("{{ hmailserver_obs_base }}/{{ hmailserver_obs_repository }}/repodata/repomd.xml.key",
                 "hmailserver_obs_fingerprint"),
     "the Build Service's key held to its fingerprint", (), ("ansible.builtin.include_tasks", "vars")),
    ("install-obs.yml", "Name the Build Service repository, its metadata and packages checked against that key",
     ("    gpgcheck: true", "    repo_gpgcheck: true"),
     "the Build Service's metadata and packages checked against its key", (), ("ansible.builtin.yum_repository",)),
    ("install-ppa.yml", "Fetch and check the PPA's key",
     (("  when: >-\n    hmailserver_ppa_key_present.rc != 0 or\n"
       "    (hmailserver_ppa_key_present.stdout | regex_findall('(?m)^pub:') | length) != 1 or\n"
       "    (hmailserver_ppa_key_present.stdout | regex_findall('(?m)^fpr:+([0-9A-F]{40}):') | first | default(''))\n"
       "      != hmailserver_ppa_fingerprint", 2), ("  block:", 4)),
     "the PPA's key fetched and checked unless the one in place is that key alone", ("when",), ("block", "always")),
    ("install-ppa.yml", "Read the fetched key's fingerprint",
     ('      ansible.builtin.command:\n'
      '        argv: [gpg, --batch, --show-keys, --with-colons, "{{ hmailserver_ppa_key_download.path }}"]',),
     "gpg reading the PPA's key fetched", (), (COMMAND, "register", "changed_when")),
    ("install-ppa.yml", "Refuse a key that is not the PPA's",
     (("      ansible.builtin.assert:\n        that:\n"
       "          - (hmailserver_ppa_key_fetched.stdout | regex_findall('(?m)^pub:') | length) == 1\n"
       "          - >-\n"
       "            (hmailserver_ppa_key_fetched.stdout | regex_findall('(?m)^fpr:+([0-9A-F]{40}):') | first | default(''))\n"
       "            == hmailserver_ppa_fingerprint", 8),),
     "the PPA's key read as its one key alone", (), (ASSERT,)),
    ("install-ppa.yml", "Put the key where the source names it",
     ('      ansible.builtin.copy:\n        src: "{{ hmailserver_ppa_key_download.path }}"\n'
      '        dest: "{{ hmailserver_ppa_keyring }}"',),
     "the PPA's key put in place as it was read", (), ("ansible.builtin.copy",)),
    ("install-ppa.yml", "Name the PPA as an apt source signed by that key alone",
     ("  ansible.builtin.template:\n    src: hmailserver-ppa.sources.j2\n"
      "    dest: /etc/apt/sources.list.d/hmailserver-ppa.sources",),
     "the PPA's source written from its template (PPA_SOURCE)", (), ("ansible.builtin.template", "register")),
    ("install-deb.yml", "Refuse a .deb whose SHA-256 is not pinned",
     (("  ansible.builtin.assert:\n    that:\n      - hmailserver_deb_checksum is match('^[0-9a-f]{64}$')", 4),),
     "the .deb's SHA-256 pinned", (), (ASSERT,)),
    ("install-deb.yml", "Download and install the .deb", (("  block:", 4),),
     "the .deb downloaded and installed as one block", ("when",), ("vars", "block", "always")),
    ("install-deb.yml", "Download the .deb and hold it to its SHA-256",
     ('        dest: "/var/cache/hmailserver-ansible/{{ hmailserver_deb_file }}"',
      '        checksum: "sha256:{{ hmailserver_deb_checksum }}"'),
     "the .deb held to its SHA-256", (), ("ansible.builtin.get_url",)),
    ("install-deb.yml", "Install the .deb with its dependencies from Debian's archive",
     ('        deb: "/var/cache/hmailserver-ansible/{{ hmailserver_deb_file }}"',),
     "the .deb installed as it was downloaded", (), ("ansible.builtin.apt",)),
    ("configure.yml", "Give the configuration the database's password", ("  no_log: true",),
     "the database's password not logged", None, ("community.general.ini_file", "no_log", "notify")),
    ("configure.yml", "Read the configuration, to see whether it has an administrator password", ("  no_log: true",),
     "the configuration's text not logged", None, (SLURP, "register", "no_log")),
    ("configure.yml", "Set the administrator password, as a PBKDF2 hash",
     (("  ansible.builtin.command:\n    argv: [hmailserver, --set-admin-password]\n"
       '    stdin: "{{ hmailserver_admin_password }}"', 2), "  no_log: true"),
     "the administrator password on standard input, not logged", None,
     (COMMAND, "when", "changed_when", "no_log", "notify")),
    ("configure.yml", "Refuse a configuration the service does not read as written",
     (("  ansible.builtin.assert:\n    that:\n"
       "      - hmailserver_check_config.stdout is search('(?m)^Database type:[ ]+' ~ hmailserver_database_type_number"
       " ~ '$')\n"
       "      - hmailserver_check_config.stdout is search('(?m)^Data directory:[ ]+\\S')", 4),
      ("  vars:\n    hmailserver_database_type_number: \"{{ 3 if hmailserver_database_type == 'PostgreSQL' else 1 }}\"",
       2)),
     "the configuration read as written, with its database type and a data directory", (), (ASSERT, "vars")),
    ("configure.yml", "Read the configuration's settings before they are stored", ("  no_log: true",),
     "the configuration's text not logged", None, (SLURP, "register", "when", "no_log")),
    ("configure.yml", "Store the settings",
     (SETTINGS_COMMAND, SETTINGS_LOOP, ('  loop_control:\n    label: "{{ item.key }}"', 2), "  failed_when: false",
      "  no_log: true"),
     "the settings' values on standard input, not logged", ("failed_when",),
     (COMMAND, "loop", "loop_control", "register", "changed_when", "vars", "no_log", "notify")),
    ("configure.yml", "Refuse a setting the server did not store",
     (("  ansible.builtin.fail:", 4),
      "  when: hmailserver_setting.results | default([]) | rejectattr('rc', 'eq', 0) | list | length > 0"),
     "a setting the server refused refused", ("when",), ("ansible.builtin.fail",)),
    ("objects.yml", "Wait for the REST API", ("  no_log: true",), "the administrator's sign-in not logged", None,
     ("ansible.builtin.uri", "register", "until", "retries", "delay", "no_log")),
    ("objects.yml", "Ask for each account", ("  no_log: true",), "the accounts' passwords not logged", None,
     ("ansible.builtin.uri", "loop", "loop_control", "register", "until", "retries", "delay", "no_log")),
    ("objects.yml", "Make each account the server does not have", ("  no_log: true",),
     "the accounts' passwords not logged", None,
     ("ansible.builtin.uri", "loop", "loop_control", "when", "changed_when", "no_log")),
)
# The arguments each module that installs, fetches, writes a source or checks may be
# given, wherever the role's tasks call it, each once and as a mapping of its own lines
# (module_problems()): any other - apt's allow_unauthenticated or force (which implies
# it), dnf's or zypper's disable_gpg_check, which ansible.builtin.package hands to them,
# zypper's extra_args, a repository's sslverify, get_url's validate_certs - is refused.
MODULE_ARGS = {
    "apt": ("name", "state", "update_cache", "cache_valid_time", "deb"),
    "package": ("name", "state"),
    "dnf": ("name", "state"),
    "dnf5": ("name", "state"),
    "yum": ("name", "state"),
    "zypper": ("name", "state", "update_cache"),
    "yum_repository": ("name", "description", "baseurl", "gpgcheck", "repo_gpgcheck", "skip_if_unavailable", "enabled",
                       "reposdir", "owner", "group", "mode"),
    "get_url": ("url", "dest", "checksum", "mode", "force", "owner", "group"),
    "rpm_key": ("key", "fingerprint", "state"),
    "template": ("src", "dest", "owner", "group", "mode"),
    "copy": ("src", "dest", "remote_src", "owner", "group", "mode"),
    "assert": ("that", "fail_msg", "success_msg", "quiet"),
    "command": ("argv", "stdin", "creates"),
}
MODULE_LINE = re.compile(r"^( *)(- +)?(['\"]?)(?:(?:ansible\.(?:builtin|legacy)|community\.general)\.)?(%s)\3[ \t]*:(.*)$"
                         % "|".join(sorted(MODULE_ARGS, key=len, reverse=True)))
# A mapping key on a line of its own, bare or quoted.
KEY_LINE = re.compile(r"(['\"]?)([A-Za-z_][\w.]*)\1[ \t]*:(?:[ \t]|$)")
# The names the held checks read that a task registers or sets: each is registered or
# set by one task alone, since a later task registering or setting it again (set_fact
# and register share one precedence, the later winning) would hand the check another
# value. And the names of the pins themselves, the role's vars/ and release.yml, which a
# set_fact would outrank: no task sets them.
CHECKED_NAMES = ("hmailserver_rpm_key_work", "hmailserver_rpm_key_fetched", "hmailserver_rpm_key_read",
                 "hmailserver_rpm_key_exported", "hmailserver_rpm_key_export_read", "hmailserver_ppa_key_download",
                 "hmailserver_ppa_key_fetched", "hmailserver_check_config", "hmailserver_setting",
                 "hmailserver_deb_checksum", "hmailserver_deb_file")
PINNED_NAMES = ("hmailserver_ppa_fingerprint", "hmailserver_copr_fingerprint", "hmailserver_obs_fingerprint",
                "hmailserver_ppa_keyring", "hmailserver_release_version", "hmailserver_release_deb_sha256")
# The PPA's apt source, its template held whole: signed by the PPA's keyring, with no
# Trusted:, no other option and nothing a Jinja expression could add a line with.
PPA_TEMPLATE = os.path.join("roles", "hmailserver", "templates", "hmailserver-ppa.sources.j2")
PPA_SOURCE = """{# https://www.progressiverobot.com #}
{# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd #}
{# SPDX-License-Identifier: AGPL-3.0-or-later #}
# {{ ansible_managed }}
# ppa:hmailserver/hmailserver: Progressive Robot's fork of hMailServer, signed by
# the PPA's key {{ hmailserver_ppa_fingerprint }} alone.
Types: deb
URIs: {{ hmailserver_ppa_uri }}
Suites: {{ ansible_facts['distribution_release'] }}
Components: main
Signed-By: {{ hmailserver_ppa_keyring }}
"""
# What switches a package manager's verification off, in any task or template of the
# role, as an argument, an option or an apt, dnf or zypper setting.
TRUST_OFF = (r"(?i)disable[-_]gpg[-_]check|allow[-_]?unauthenticated|nogpgcheck|no-gpg-checks|gpg-auto-import-keys"
             r"|auto_import_keys|--nosignature|--nodigest|force-yes|force-bad-verify|allow[-_]?insecure|allow[-_]?weak"
             r"|allow[-_]?downgrade[-_]?to[-_]?insecure|\btrusted\b|gpgcheck[ \t]*=[ \t]*(?:0|false|no)\b|--setopt\b")
# What no file of the role's tasks/ may say ("*" for every one of them, but those named
# after it), each a pattern and why. A key keyword quoted ('ignore_errors': true) is
# the same keyword.
RPM_KEY_MODULE = r"(?m)^[ \t-]*['\"]?(?:ansible\.(?:builtin|legacy)\.)?rpm_key['\"]?[ \t]*:"
FORBIDDEN = (
    ("*", r"(?m)^[ \t-]*['\"]?gpgkey['\"]?[ \t]*:",
     "a repository names a gpgkey, through which dnf or zypper would import any key there", ()),
    ("*", RPM_KEY_MODULE, "a key is given to rpm_key itself, not read first as one key alone and exported (rpm-key.yml)",
     ("rpm-key.yml",)),
    ("*", r"\brpm(?:keys)?\b[^\n]*--import", "a key is given to rpm --import itself (rpm-key.yml)", ()),
    ("*", r"(?m)^[ \t-]*['\"]?(?:ignore_errors|ignore_unreachable)['\"]?[ \t]*:",
     "a task's failure is ignored, and with it a check's", ()),
    ("*", r"(?m)^[ \t-]*['\"]?rescue['\"]?[ \t]*:", "a block rescues its failures, and with them a check's", ()),
    ("*", r"(?m)^[ \t-]*['\"]?apply['\"]?[ \t]*:", "an include gives the tasks it includes keywords of its own", ()),
    ("*", r"(?m)^[ \t-]*['\"]?(?:args|action|local_action)['\"]?[ \t]*:",
     "a task gives its module arguments by args or action, which MODULE_ARGS does not read", ()),
    ("*", r"(?m)^[ \t-]*['\"]?(?:(?:ansible\.(?:builtin|legacy)|community\.general)\.)?"
          r"(?:apt_repository|deb822_repository|apt_key|zypper_repository|copr)['\"]?[ \t]*:",
     "a source or key is written by a module of its own, not by the role's held template and repositories", ()),
    ("*", TRUST_OFF, "a package manager's verification is switched off", ()),
)


def held(block, lines):
    """Whether block holds lines whole: the text as written, each line with its
    indentation, and the next line that is not blank indented no deeper than the
    text's last line, or than the number lines is given with as (text, deepest) - for
    a text that ends in a block scalar, whose lines go on at its own indentation, or
    in a key whose value follows it."""
    text, deepest = lines if isinstance(lines, tuple) else (lines, None)
    last = text.split("\n")[-1]
    limit = len(last) - len(last.lstrip(" ")) if deepest is None else deepest
    start = 0
    while True:
        at = block.find("\n" + text + "\n", start)
        if at < 0:
            return False
        following = next((line for line in block[at + len(text) + 2:].split("\n") if line.strip()), "")
        if len(following) - len(following.lstrip(" ")) <= limit:
            return True
        start = at + 1


def task_keywords(block, keyword):
    """The lines of the task in block (task_block's) that give it keyword, quoted or not."""
    first = block.split("\n")[1]
    indent = len(first) - len(first.lstrip(" ")) + 2
    return re.findall(r"(?m)^ {%d}['\"]?%s['\"]?[ \t]*:.*$" % (indent, re.escape(keyword)), block)


def task_keys(block):
    """The keywords of the task in block (task_block's) at its own indentation, in
    order, name first, and the lines there that are none: a comment, or an item of a
    sequence a keyword's value goes on in, is neither."""
    lines = block.split("\n")
    first = lines[1]
    indent = len(first) - len(first.lstrip(" ")) + 2
    keys, odd = ["name"], []
    for line in lines[2:]:
        depth = len(line) - len(line.lstrip(" "))
        if not line.strip() or depth > indent:
            continue
        text = line[depth:]
        if depth == indent and (text.startswith("#") or text.startswith("- ") or text == "-"):
            continue
        match = KEY_LINE.match(text) if depth == indent else None
        if match:
            keys.append(match.group(2))
        else:
            odd.append(line.strip())
    return keys, odd


def module_problems(name, text):
    """What a file of the role's tasks gives the modules of MODULE_ARGS that they may not
    be given: an argument not named there, one given twice, a line among them that is no
    argument, or arguments on the module's own line, which the check would not read."""
    problems = []
    lines = text.split("\n")
    for number, line in enumerate(lines):
        match = MODULE_LINE.match(line)
        if not match:
            continue
        module = match.group(4)
        column = len(match.group(1)) + len(match.group(2) or "")
        where = "%s, line %d" % (os.path.join(TASKS, name), number + 1)
        after = match.group(5).strip()
        if after and not after.startswith("#"):
            problems.append("%s gives %s its arguments on the module's own line, %r: they are a mapping of their own"
                            " lines, each of them one MODULE_ARGS lets it be given" % (where, module, after))
            continue
        arguments, depth_of, odd = [], None, []
        for following in lines[number + 1:]:
            depth = len(following) - len(following.lstrip(" "))
            if not following.strip():
                continue
            if depth <= column:
                break
            body = following[depth:]
            if depth_of is None:
                if body.startswith("#"):
                    continue
                depth_of = depth
            if depth > depth_of:
                continue
            if depth < depth_of:
                odd.append(body)
                continue
            if body.startswith("#") or body.startswith("- ") or body == "-":
                continue
            key = KEY_LINE.match(body)
            if key:
                arguments.append(key.group(2))
            else:
                odd.append(body)
        stray = [a for a in arguments if a not in MODULE_ARGS[module]]
        twice = sorted(set(a for a in arguments if arguments.count(a) > 1))
        if stray or twice or odd:
            problems.append("%s gives %s %s, which it may not be given (MODULE_ARGS: %s)" % (
                where, module, ", ".join(stray + ["%s twice" % a for a in twice] + ["the line %r" % o for o in odd]),
                ", ".join(MODULE_ARGS[module])))
    return problems


def has_heading(changelog, version):
    """Whether the collection's CHANGELOG.md has the heading "## <version>"."""
    return re.search(r"(?m)^## %s$" % re.escape(version), changelog) is not None


def say(line=""):
    print(line)
    sys.stdout.flush()


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read().replace("\r\n", "\n")


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def tarball_name(version):
    return "%s-%s-%s.tar.gz" % (NAMESPACE, NAME, version)


# ---------------------------------------------------------------- what the files say

def galaxy_version(text):
    found = re.findall(r"(?m)^version: ([^\s#]+)\s*$", text)
    if len(found) != 1:
        raise Refused("%s has %d version lines, not one" % (GALAXY_YML, len(found)))
    return found[0]


def release_pins(text):
    """(version, {target: sha256}) of vars/release.yml."""
    versions = re.findall(r'(?m)^hmailserver_release_version: "([^"]*)"\s*$', text)
    if len(versions) != 1:
        raise Refused("%s has %d hmailserver_release_version lines, not one" % (RELEASE_YML, len(versions)))
    pins = {}
    for target, value in re.findall(r"(?m)^  (debian[0-9]+_[a-z0-9]+): (\S+)\s*$", text):
        if target in pins:
            raise Refused("%s pins %s twice" % (RELEASE_YML, target))
        pins[target] = value
    return versions[0], pins


def render(directory, version, pins):
    """galaxy.yml's version and vars/release.yml's version and pins, written for the
    release, in a staged copy."""
    path = os.path.join(directory, GALAXY_YML)
    text = read(path)
    galaxy_version(text)
    write(path, re.sub(r"(?m)^version: [^\s#]+\s*$", "version: " + version, text))
    path = os.path.join(directory, RELEASE_YML)
    text = read(path)
    release_pins(text)
    text = re.sub(r'(?m)^hmailserver_release_version: "[^"]*"\s*$', 'hmailserver_release_version: "%s"' % version, text)
    for target in TARGETS:
        text, count = re.subn(r"(?m)^  %s: \S+\s*$" % re.escape(target), "  %s: %s" % (target, pins[target]), text)
        if count != 1:
            raise Refused("%s has no single pin line for %s" % (RELEASE_YML, target))
    write(path, text)


def pins_from_sums(text, version):
    """{target: sha256} of the release's .deb files for Debian, from its SHA256SUMS."""
    listed = {}
    for line in text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64}) [ *](\S+)", line.strip())
        if match:
            if match.group(2) in listed:
                raise Refused("%s lists %s twice" % (SUMS % version, match.group(2)))
            listed[match.group(2)] = match.group(1)
    pins = {}
    for target in TARGETS:
        name = "hmailserver_%s-1~%s.deb" % (version, target)
        if name not in listed:
            raise Refused("%s does not list %s: the release carries no .deb the role installs on %s"
                          % (SUMS % version, name, target))
        pins[target] = listed[name]
    return pins


# ---------------------------------------------------------------- check

def task_block(text, name):
    """The lines of the task called name in a file of tasks, from its - name: line to the
    next line indented no deeper than that one, each ending in a line feed and the first
    after one; None when no task has that name, or more than one."""
    found = list(re.finditer(r"(?m)^([ ]*)- name: %s[ \t]*$" % re.escape(name), text))
    if len(found) != 1:
        return None
    indent = len(found[0].group(1))
    lines = [found[0].group(0)]
    for line in text[found[0].end():].split("\n")[1:]:
        if line.strip() and len(line) - len(line.lstrip(" ")) <= indent:
            break
        lines.append(line)
    return "\n" + "\n".join(lines) + "\n"


def check(root=ROOT):
    """The problems of the tree's collection; an empty list when it holds together."""
    base = os.path.join(root, COLLECTION)
    problems = []
    galaxy = read(os.path.join(base, GALAXY_YML))
    for key, value in (("namespace", NAMESPACE), ("name", NAME), ("repository", REPOSITORY), ("issues", ISSUES)):
        if not re.search(r"(?m)^%s: %s\s*$" % (key, re.escape(value)), galaxy):
            problems.append("%s: %s is not %s" % (GALAXY_YML, key, value))
    if not re.search(r"(?m)^license:\n  - %s[ \t]*(?:#.*)?$" % re.escape(LICENCE), galaxy) or re.search(
            r"(?m)^license:\n  - .*$" + FURTHER_ITEM, galaxy):
        problems.append("%s: the licence is not %s alone" % (GALAXY_YML, LICENCE))
    # The lines indented two after the key alone: the folded block ends at the first
    # line that is not, and a comment after it is not the description.
    description = re.search(r"(?m)^description: >-\n((?:  .*\n)+)", galaxy)
    words = " ".join(description.group(1).split()) if description else ""
    if FORK not in words or " ".join(NOT_ORIGINAL.split()) not in words:
        problems.append("%s: the description does not say %s, not the original hMailServer" % (GALAXY_YML, FORK))
    meta = " ".join(read(os.path.join(base, ROLE_META)).split())
    if FORK not in meta or " ".join(NOT_ORIGINAL.split()) not in meta:
        problems.append("%s: the role's description does not say %s, not the original hMailServer" % (ROLE_META, FORK))
    try:
        version = galaxy_version(galaxy)
        pinned, pins = release_pins(read(os.path.join(base, RELEASE_YML)))
    except Refused as error:
        problems.append(str(error))
        version, pinned, pins = None, None, {}
    if version is not None and not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        problems.append("%s: version %s is not a release version, X.Y.Z" % (GALAXY_YML, version))
    if version is not None and not has_heading(read(os.path.join(base, CHANGELOG)), version):
        problems.append("%s has no heading \"## %s\" for the version %s carries: what a version carries is written"
                        " there before it is built" % (CHANGELOG, version, GALAXY_YML))
    if version is not None and pinned != version:
        problems.append("%s pins the .deb files of %s, and %s says %s: a collection is built for one release"
                        % (RELEASE_YML, pinned, GALAXY_YML, version))
    if sorted(pins) != sorted(TARGETS):
        problems.append("%s pins %s, not %s" % (RELEASE_YML, ", ".join(sorted(pins)) or "nothing", ", ".join(TARGETS)))
    for target, value in sorted(pins.items()):
        if not re.fullmatch(r"[0-9a-f]{64}", value):
            problems.append("%s: the pin of %s is not a SHA-256: %s" % (RELEASE_YML, target, value))
    main_vars = read(os.path.join(base, MAIN_VARS))
    for key in ("hmailserver_ppa_fingerprint", "hmailserver_copr_fingerprint", "hmailserver_obs_fingerprint"):
        found = re.findall(r"(?m)^%s: (\S+)\s*$" % key, main_vars)
        if len(found) != 1 or not re.fullmatch(r"[0-9A-F]{40}", found[0]):
            problems.append("%s: %s is not one fingerprint of 40 hex digits" % (MAIN_VARS, key))
    if "search=0x" + (re.findall(r"(?m)^hmailserver_ppa_fingerprint: (\S+)", main_vars) or ["?"])[0] not in main_vars:
        problems.append("%s: the PPA's key is not fetched by the fingerprint it is held to" % MAIN_VARS)
    defaults = read(os.path.join(base, DEFAULTS))
    for key in ("hmailserver_database_password", "hmailserver_admin_password"):
        if re.search(r"(?m)^%s:" % key, defaults):
            problems.append("%s gives %s a default: it is required, and the role's argument spec says so" % (DEFAULTS, key))
    directory = os.path.join(base, TASKS)
    listed = os.listdir(directory) if os.path.isdir(directory) else []
    tasks = {}
    for name in sorted(set(n for n in listed if n.endswith((".yml", ".yaml"))) | set(entry[0] for entry in VERIFIED)):
        try:
            tasks[name] = read(os.path.join(directory, name))
        except OSError:
            tasks[name] = ""
    for name, task, lines, what, control, keys in VERIFIED:
        block = task_block(tasks[name], task)
        if block is None:
            problems.append("%s has no task %r: %s is not checked" % (os.path.join(TASKS, name), task, what))
            continue
        if not all(held(block, text) for text in lines):
            problems.append("%s's task %r does not hold %s" % (os.path.join(TASKS, name), task, what))
        for keyword in CONTROL_KEYS if control is not None else ():
            given = task_keywords(block, keyword)
            if given and (keyword not in control or len(given) > 1):
                problems.append("%s's task %r is given %s, which can skip it or take its failure for a success: %s is"
                                " not held" % (os.path.join(TASKS, name), task, given[0].strip(), what))
        found, odd = task_keys(block)
        allowed = set(("name",) + tuple(keys) + (CONTROL_KEYS if control is not None else ()))
        stray = [k for k in found if k not in allowed]
        twice = sorted(set(k for k in found if found.count(k) > 1 and (control is None or k not in CONTROL_KEYS)))
        if stray or twice or odd:
            problems.append("%s's task %r carries %s, which the check does not let it carry: %s is not held" % (
                os.path.join(TASKS, name), task,
                ", ".join(stray + ["%s twice" % k for k in twice] + ["the line %r" % line for line in odd]), what))
    block = task_block(tasks[SETTINGS_TASK[0]], SETTINGS_TASK[1])
    if block is not None:
        # The task but its command and its changed_when, which alone may name a value.
        rest = block.replace("\n" + SETTINGS_COMMAND[0] + "\n", "\n", 1).replace("\n" + SETTINGS_LOOP + "\n", "\n", 1)
        rest = re.sub(r"(?m)^  changed_when:.*\n(?:(?:[ ]{3,}.*)?\n)*", "", rest)
        if (re.search(SETTING_VALUE, rest) or re.search(r"\b(?:lookup|query|q)\s*\(", block)
                or re.search(r"(?<![\w.])hmailserver_settings(?!\w)", rest)
                or re.search(r"\b(?:vars|hostvars)\s*[\[.]", block)):
            problems.append("%s's task %r names a setting's value outside its standard input, or runs a lookup: a"
                            " value would be given on a command line or shown" % (os.path.join(TASKS, SETTINGS_TASK[0]),
                                                                                  SETTINGS_TASK[1]))
    for name, pattern, what, spared in FORBIDDEN:
        for each in sorted(tasks) if name == "*" else (name,):
            if each not in spared and re.search(pattern, tasks.get(each, "")):
                problems.append("%s: %s" % (os.path.join(TASKS, each), what))
    if len(re.findall(RPM_KEY_MODULE, tasks.get("rpm-key.yml", ""))) != 1:
        problems.append("%s gives rpm_key a key more than once, or never: one import, of gpg's export of the key alone"
                        % os.path.join(TASKS, "rpm-key.yml"))
    for name in sorted(tasks):
        problems.extend(module_problems(name, tasks[name]))
    every = "\n".join(tasks[name] for name in sorted(tasks))
    for name in CHECKED_NAMES + PINNED_NAMES:
        count = len(re.findall(r"(?m)^[ \t-]*['\"]?register['\"]?[ \t]*:[ \t]*['\"]?%s['\"]?[ \t]*(?:#.*)?$" % name, every))
        count += len(re.findall(r"(?m)(?<![\w.])['\"]?%s['\"]?[ \t]*(?::(?=[ \t]|$)|=(?!=))" % name, every))
        wanted = 1 if name in CHECKED_NAMES else 0
        if count != wanted:
            problems.append("the role's tasks register or set %s %s, not %s: %s" % (
                name, {0: "never", 1: "once"}.get(count, "%d times" % count), "once" if wanted else "never",
                "a held check reads it, and a later task registering or setting it would hand the check another value"
                if wanted else "it is a pin, and a set_fact would outrank it"))
    templates = os.path.join(base, os.path.dirname(PPA_TEMPLATE))
    for name in sorted(os.listdir(templates)) if os.path.isdir(templates) else []:
        try:
            template = read(os.path.join(templates, name))
        except (OSError, UnicodeDecodeError):
            continue
        if re.search(TRUST_OFF, template):
            problems.append("%s: a package manager's verification is switched off" % os.path.join(
                os.path.dirname(PPA_TEMPLATE), name))
    try:
        source = read(os.path.join(base, PPA_TEMPLATE))
    except OSError:
        source = None
    if source != PPA_SOURCE:
        problems.append("%s is not the PPA's source as held (PPA_SOURCE): signed by the PPA's keyring alone, with nothing"
                        " more" % PPA_TEMPLATE)
    for directory, names, files in os.walk(base):
        names[:] = [n for n in names if n not in SKIP]
        for name in files:
            path = os.path.join(directory, name)
            try:
                text = read(path)
            except UnicodeDecodeError:
                problems.append("%s is not UTF-8 text" % os.path.relpath(path, root))
                continue
            if re.search(r"github(usercontent)?\.com|ghcr\.io", text, re.IGNORECASE):
                problems.append("%s names GitHub: the collection takes nothing from there" % os.path.relpath(path, root))
    return problems


def check_command(root=ROOT):
    problems = check(root)
    for problem in problems:
        say("FAILED: " + problem)
    if problems:
        return 1
    version, pins = release_pins(read(os.path.join(root, COLLECTION, RELEASE_YML)))
    say("The collection %s.%s %s holds together: built for the release %s, with %d .deb pins."
        % (NAMESPACE, NAME, version, version, len(pins)))
    return 0


# ---------------------------------------------------------------- build

def stage(into, root=ROOT):
    """The tree's collection copied to into/ansible_collections/progressiverobot/hmailserver,
    with the repository's LICENSE (LF), as ansible-galaxy builds it; answers that path."""
    target = os.path.join(into, "ansible_collections", NAMESPACE, NAME)
    shutil.copytree(os.path.join(root, COLLECTION), target, ignore=shutil.ignore_patterns(*SKIP))
    write(os.path.join(target, "LICENSE"), read(os.path.join(root, "LICENSE")))
    return target


def inspect(tarball, version, pins):
    """The built tarball read back; Refused unless it is the collection, at the version
    and with the pins it was built with, carrying LICENSE."""
    with tarfile.open(tarball, "r:gz") as archive:
        names = archive.getnames()
        for name in names:
            if name.startswith("/") or ".." in name.split("/"):
                raise Refused("%s holds the path %s" % (os.path.basename(tarball), name))
        manifest = json.load(archive.extractfile("MANIFEST.json"))
        release = archive.extractfile(RELEASE_YML.replace(os.sep, "/")).read().decode("utf-8")
        has_licence = "LICENSE" in names
    info = manifest.get("collection_info") or {}
    wanted = {"namespace": NAMESPACE, "name": NAME, "version": version, "license": [LICENCE]}
    for key, value in wanted.items():
        if info.get(key) != value:
            raise Refused("%s's MANIFEST.json gives %s %r, not %r" % (os.path.basename(tarball), key, info.get(key), value))
    if not has_licence:
        raise Refused("%s carries no LICENSE" % os.path.basename(tarball))
    built_version, built_pins = release_pins(release)
    if built_version != version or built_pins != pins:
        raise Refused("%s's %s pins %s %s, not %s %s" % (os.path.basename(tarball), RELEASE_YML, built_version,
                                                         built_pins, version, pins))
    return len(names)


def build(out, version=None, pins=None, root=ROOT, run=subprocess.run, galaxy="ansible-galaxy"):
    """The collection built into out; answers (tarball path, version, pins, its SHA-256).
    Without a version, the tree's own version and pins."""
    work = tempfile.mkdtemp(prefix="hm-galaxy-")
    try:
        staged = stage(work, root)
        if version is not None:
            render(staged, version, pins)
        version = galaxy_version(read(os.path.join(staged, GALAXY_YML)))
        built_version, pins = release_pins(read(os.path.join(staged, RELEASE_YML)))
        if built_version != version:
            raise Refused("%s is %s and %s pins %s" % (GALAXY_YML, version, RELEASE_YML, built_version))
        os.makedirs(out, exist_ok=True)
        tarball = os.path.join(out, tarball_name(version))
        if os.path.exists(tarball):
            os.remove(tarball)
        answer = run([galaxy, "collection", "build", "--output-path", out, staged], capture_output=True, text=True)
        said = ((answer.stdout or "") + (answer.stderr or "")).strip()
        if answer.returncode != 0 or not os.path.isfile(tarball):
            raise Refused("ansible-galaxy collection build failed (exit %s): %s" % (answer.returncode, said[-2000:]))
        count = inspect(tarball, version, pins)
        digest = rf.sha256_file(tarball)
        say("built %s, %d entries, SHA-256 %s" % (os.path.basename(tarball), count, digest))
        return tarball, version, pins, digest
    finally:
        shutil.rmtree(work, ignore_errors=True)


def release_build(out, environ=None, root=ROOT, run=subprocess.run):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    changelog = os.path.join(root, COLLECTION, CHANGELOG)
    if not has_heading(read(changelog) if os.path.isfile(changelog) else "", version):
        raise Refused("the collection's %s has no heading \"## %s\": what the collection built for %s carries is written"
                      " there, on master, before it is built (hmailserver/docs/Ansible.md, \"Publishing to Galaxy\");"
                      " nothing was fetched or built" % (CHANGELOG, version, version))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    cosign = rf.tool_path("cosign", out, environ)
    sums = SUMS % version
    record = rf.verified_files(version, [sums], os.path.join(out, "release"), cosign, fix)
    rf.write_record(record, version, where, os.path.join(out, "verification.json"))
    pins = pins_from_sums(read(record[sums]["path"]), version)
    for target in TARGETS:
        say("  %s  hmailserver_%s-1~%s.deb" % (pins[target], version, target))
    tarball, version, pins, digest = build(os.path.join(out, "dist"), version, pins, root, run)
    document = {"version": version, "tarball": os.path.basename(tarball), "sha256": digest, "pins": pins,
                "sums": {"name": sums, "sha256": record[sums]["sha256"], "identity": record[sums]["identity"]}}
    write(os.path.join(out, RECORD), json.dumps(document, indent=1, sort_keys=True) + "\n")
    say("BUILT: %s for v%s, its .deb pins from the verified %s; nothing was published." % (
        os.path.basename(tarball), version, sums))
    return 0


# ---------------------------------------------------------------- publish

def galaxy_has(version, http):
    """True when Galaxy has the version, False when it answers that it has not."""
    url = "%sv3/plugin/ansible/content/published/collections/index/%s/%s/versions/%s/" % (GALAXY_API, NAMESPACE, NAME, version)
    status, _, _ = http(url, headers={"Accept": "application/json"})
    if status == 200:
        return True
    if status == 404:
        return False
    raise Refused("Galaxy answered HTTP %s for %s: nothing was sent" % (status, url))


def publish(source, environ=None, http=None, run=subprocess.run, galaxy="ansible-galaxy"):
    environ = os.environ if environ is None else environ
    http = http or rf.http
    token = environ.get("GALAXY_API_KEY")
    if not token:
        say("not configured: GALAXY_API_KEY is not set, so nothing was sent to Ansible Galaxy.")
        say("The owner signs in to galaxy.ansible.com, has the namespace progressiverobot made his, and makes an")
        say("API token; a maintainer stores it as the protected, masked CI/CD variable GALAXY_API_KEY")
        say("(hmailserver/docs/Ansible.md, \"Publishing to Galaxy\").")
        return 0
    path = os.path.join(source, RECORD)
    if not os.path.isfile(path):
        raise Refused("%s is not there: galaxy-build, which this job follows, built nothing" % path)
    with open(path, encoding="utf-8") as f:
        document = json.load(f)
    version = document.get("version")
    fix = rf.fixture(environ)
    now, where = rf.release_version(environ, fix)
    if now != version:
        raise Refused("galaxy-build built %s and the release is now %s (from %s): nothing was sent" % (version, now, where))
    tarball = os.path.join(source, "dist", tarball_name(version))
    if document.get("tarball") != os.path.basename(tarball) or not os.path.isfile(tarball):
        raise Refused("galaxy-build's tarball, %s, is not there" % os.path.basename(tarball))
    digest = rf.sha256_file(tarball)
    if digest != document.get("sha256"):
        raise Refused("%s is not the tarball galaxy-build recorded (SHA-256 %s, not %s): nothing was sent"
                      % (os.path.basename(tarball), digest, document.get("sha256")))
    inspect(tarball, version, document.get("pins"))
    if galaxy_has(version, http):
        say("Galaxy has %s.%s %s already: nothing was sent (a version is kept for good)." % (NAMESPACE, NAME, version))
        return 0
    child = dict(environ)
    child.pop("GALAXY_API_KEY", None)
    child["ANSIBLE_GALAXY_SERVER_LIST"] = SERVER_ID
    child["ANSIBLE_GALAXY_SERVER_%s_URL" % SERVER_ID.upper()] = GALAXY_API
    child["ANSIBLE_GALAXY_SERVER_%s_TOKEN" % SERVER_ID.upper()] = token
    command = [galaxy, "collection", "publish", "--import-timeout", "900", tarball]
    answer = run(command, capture_output=True, text=True, env=child)
    child = None
    token_free = ((answer.stdout or "") + (answer.stderr or "")).replace(token, "(the token)").strip()
    token = None
    if answer.returncode != 0:
        raise Refused("ansible-galaxy collection publish failed (exit %s): %s" % (answer.returncode, token_free[-2000:]))
    say(token_free)
    if not galaxy_has(version, http):
        raise Refused("ansible-galaxy said it published %s, and Galaxy does not list it" % version)
    say("PUBLISHED: %s.%s %s on Ansible Galaxy: %s/ui/repo/published/%s/%s/" % (NAMESPACE, NAME, version, GALAXY,
                                                                              NAMESPACE, NAME))
    return 0


# ---------------------------------------------------------------- self-test

class Answer:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def fake_build(calls):
    """ansible-galaxy collection build as far as this script reads it: a tarball of the
    staged directory with the MANIFEST.json it would write."""
    def run(command, capture_output=True, text=True, env=None):
        calls.append(command)
        out, staged = command[4], command[5]
        version = galaxy_version(read(os.path.join(staged, GALAXY_YML)))
        manifest = {"collection_info": {"namespace": NAMESPACE, "name": NAME, "version": version, "license": [LICENCE]}}
        with tarfile.open(os.path.join(out, tarball_name(version)), "w:gz") as archive:
            data = json.dumps(manifest).encode("utf-8")
            info = tarfile.TarInfo("MANIFEST.json")
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
            for directory, _, files in os.walk(staged):
                for name in files:
                    if name == GALAXY_YML:
                        continue
                    path = os.path.join(directory, name)
                    archive.add(path, os.path.relpath(path, staged).replace(os.sep, "/"))
        return Answer()
    return run


def self_test():
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    def refused(what, call, needle):
        try:
            call()
        except Refused as error:
            expect("%s (%s)" % (what, str(error).splitlines()[0][:110]), needle in str(error))
            return
        expect(what + ": not refused", False)

    expect("the tree's collection holds together", check() == [])
    held_say = globals()["say"]
    work = tempfile.mkdtemp(prefix="hm-galaxy-test-")
    try:
        globals()["say"] = lambda line="": None

        def broken(what, relative, old, new, needle, also=()):
            copy = os.path.join(work, "tree-%d" % len(os.listdir(work)))
            os.makedirs(os.path.join(copy, os.path.dirname(COLLECTION)))
            shutil.copytree(os.path.join(ROOT, COLLECTION), os.path.join(copy, COLLECTION),
                            ignore=shutil.ignore_patterns(*SKIP))
            path = os.path.join(copy, COLLECTION, relative)
            text = read(path)
            for before, after in ((old, new),) + tuple(also):
                if before not in text:
                    expect("%s: the text to break is not in %s" % (what, relative), False)
                    return
                text = text.replace(before, after, 1)
            write(path, text)
            found = check(copy)
            expect("%s is caught" % what, any(needle in p for p in found))

        version, pins = release_pins(read(os.path.join(ROOT, COLLECTION, RELEASE_YML)))
        broken("galaxy.yml at another version than the pins", GALAXY_YML, "version: %s" % version, "version: 9.9.9",
               "a collection is built for one release")
        broken("a description that does not say fork", GALAXY_YML, "Progressive Robot's fork of hMailServer",
               "hMailServer", "does not say")
        broken("a description that does not say not the original", GALAXY_YML, "not the\n  original", "and the\n  original",
               "does not say")
        broken("the role's description without the fork", ROLE_META, "Progressive Robot's fork", "a build", "role's description")
        broken("another namespace", GALAXY_YML, "namespace: progressiverobot", "namespace: hmailserver", "namespace")
        broken("a second licence", GALAXY_YML, "  - AGPL-3.0-or-later\n", "  - AGPL-3.0-or-later\n  - MIT\n", "licence")
        broken("a second licence after a comment", GALAXY_YML, "  - AGPL-3.0-or-later\n",
               "  - AGPL-3.0-or-later\n  # and also\n  - MIT\n", "licence")
        broken("a second licence after a blank line", GALAXY_YML, "  - AGPL-3.0-or-later\n",
               "  - AGPL-3.0-or-later\n\n  - MIT\n", "licence")
        broken("a description that says neither, the words in a comment at the end of galaxy.yml", GALAXY_YML,
               "  Installs and configures Progressive Robot's fork of hMailServer - not the\n  original hMailServer - on Linux,",
               "  Installs and configures hMailServer on Linux,", "does not say",
               also=(("  - tests/output\n", "  - tests/output\n# Progressive Robot's fork of hMailServer - not the original"
                      " hMailServer\n"),))
        rpm_key = os.path.join(TASKS, "rpm-key.yml")
        broken("an RPM channel's key imported without its fingerprint", rpm_key,
               "        fingerprint: \"{{ hmailserver_rpm_key_fingerprint }}\"\n", "", "does not hold")
        broken("an RPM channel's key file not read as one key alone", rpm_key,
               "          - (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n", "",
               "does not hold")
        broken("Copr's key given to rpm_key itself", os.path.join(TASKS, "install-copr.yml"),
               "  ansible.builtin.include_tasks: rpm-key.yml\n  vars:\n"
               "    hmailserver_rpm_key_url: \"{{ hmailserver_copr_base }}/pubkey.gpg\"\n"
               "    hmailserver_rpm_key_fingerprint: \"{{ hmailserver_copr_fingerprint }}\"\n",
               "  ansible.builtin.rpm_key:\n    key: \"{{ hmailserver_copr_base }}/pubkey.gpg\"\n"
               "    fingerprint: \"{{ hmailserver_copr_fingerprint }}\"\n", "rpm_key itself")
        broken("Copr's repository without gpgcheck", os.path.join(TASKS, "install-copr.yml"), "    gpgcheck: true\n",
               "    gpgcheck: false\n", "does not hold")
        broken("the Build Service's repository with a gpgkey", os.path.join(TASKS, "install-obs.yml"),
               "    repo_gpgcheck: true\n", "    repo_gpgcheck: true\n    gpgkey: \"{{ hmailserver_obs_base }}/x.key\"\n",
               "names a gpgkey")
        broken("the Build Service's metadata unchecked", os.path.join(TASKS, "install-obs.yml"), "    repo_gpgcheck: true\n",
               "    repo_gpgcheck: false\n", "does not hold")
        broken("the PPA's key check made true", os.path.join(TASKS, "install-ppa.yml"),
               "          - (hmailserver_ppa_key_fetched.stdout | regex_findall('(?m)^pub:') | length) == 1\n",
               "          - true\n", "does not hold")
        broken("the .deb downloaded without its checksum", os.path.join(TASKS, "install-deb.yml"),
               "        checksum: \"sha256:{{ hmailserver_deb_checksum }}\"\n", "", "does not hold")
        broken("the administrator password logged", os.path.join(TASKS, "configure.yml"),
               "    stdin: \"{{ hmailserver_admin_password }}\"\n  when: hmailserver_admin_password_update == 'always' or"
               " not hmailserver_admin_password_present\n  changed_when: true\n  no_log: true\n",
               "    stdin: \"{{ hmailserver_admin_password }}\"\n  when: hmailserver_admin_password_update == 'always' or"
               " not hmailserver_admin_password_present\n  changed_when: true\n", "does not hold")
        setting_name = "      - \"{{ item.key if (item.value | string | trim | length) > 0 else item.key ~ '=' }}\"\n"
        broken("a setting's value on the command line", os.path.join(TASKS, "configure.yml"), setting_name,
               "      - \"{{ item.key }}={{ item.value }}\"\n", "does not hold")
        broken("a setting's value on the command line, joined with ~", os.path.join(TASKS, "configure.yml"), setting_name,
               "      - \"{{ item.key ~ '=' ~ item.value }}\"\n", "does not hold")
        broken("a setting's value on the command line, by format", os.path.join(TASKS, "configure.yml"), setting_name,
               "      - \"{{ '%s=%s' | format(item.key, item.value) }}\"\n", "does not hold")
        broken("a setting's value given a further argument", os.path.join(TASKS, "configure.yml"), setting_name,
               setting_name + "      - \"{{ item.value }}\"\n", "does not hold")
        broken("a setting's value in the task's environment", os.path.join(TASKS, "configure.yml"),
               "  register: hmailserver_setting\n",
               "  register: hmailserver_setting\n  environment:\n    HM_VALUE: \"{{ item['value'] }}\"\n",
               "outside its standard input")
        broken("a setting's value through a lookup in changed_when", os.path.join(TASKS, "configure.yml"),
               "    hmailserver_setting.rc == 0 and\n",
               "    hmailserver_setting.rc == 0 and lookup('ansible.builtin.pipe', 'true ' ~ item.value) == '' and\n",
               "runs a lookup")
        refusal = "      ansible.builtin.assert:\n        that:\n          - (hmailserver_rpm_key_fetched.content"
        broken("the key file's check skipped by a when", rpm_key, "    - name: Refuse a file that is not the channel's key alone\n",
               "    - name: Refuse a file that is not the channel's key alone\n      when: false\n", "skip it")
        broken("the key file's check given failed_when", rpm_key,
               "    - name: Refuse a file that is not the channel's key alone\n",
               "    - name: Refuse a file that is not the channel's key alone\n      failed_when: false\n", "skip it")
        broken("the key file's check's failure ignored", rpm_key, refusal,
               "      ignore_errors: true\n" + refusal, "is ignored")
        broken("the key file's check's failure ignored, the keyword quoted", rpm_key, refusal,
               "      'ignore_errors': true\n" + refusal, "is ignored")
        broken("the key file's pub count made true", rpm_key,
               "          - (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n",
               "          - true or (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n",
               "does not hold")
        broken("the key file's pub count continued with or true", rpm_key,
               "          - (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n",
               "          - (hmailserver_rpm_key_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n"
               "            or true\n", "does not hold")
        broken("the key file's fingerprint check continued with or true", rpm_key,
               "            == hmailserver_rpm_key_fingerprint\n        fail_msg: >-\n          The key file fetched",
               "            == hmailserver_rpm_key_fingerprint\n            or true\n        fail_msg: >-\n"
               "          The key file fetched", "does not hold")
        broken("the key file's armour blocks not counted", rpm_key,
               "          - (hmailserver_rpm_key_fetched.content | b64decode | regex_findall('-----BEGIN PGP ') | length) == 1\n",
               "", "does not hold")
        broken("the key file's conditions moved out of that:", rpm_key,
               "        that:\n          - (hmailserver_rpm_key_fetched.content",
               "        that:\n          - true\n        msg_extra:\n          - (hmailserver_rpm_key_fetched.content",
               "does not hold")
        broken("gpg made to read another file", rpm_key,
               "--show-keys, --with-colons,\n               \"{{ hmailserver_rpm_key_work.path }}/fetched.asc\"]",
               "--show-keys, --with-colons,\n               /etc/pki/rpm-gpg/RPM-GPG-KEY-fedora]", "does not hold")
        broken("rpm_key given the file fetched, not the export", rpm_key,
               "        key: \"{{ hmailserver_rpm_key_work.path }}/key.asc\"\n",
               "        key: \"{{ hmailserver_rpm_key_work.path }}/fetched.asc\"\n", "does not hold")
        broken("the key exported with every other key", rpm_key,
               "--export, \"{{ hmailserver_rpm_key_fingerprint }}\"]", "--export]", "does not hold")
        broken("the export not read as one key alone", rpm_key,
               "          - (hmailserver_rpm_key_export_read.stdout | regex_findall('(?m)^pub:') | length) == 1\n", "",
               "does not hold")
        broken("a block that rescues the key's checks", rpm_key, "  always:\n",
               "  rescue:\n    - name: Go on\n      ansible.builtin.meta: noop\n\n  always:\n", "rescues")
        broken("a second rpm_key in rpm-key.yml", rpm_key, "  always:\n",
               "    - name: Import the file too\n      ansible.legacy.rpm_key:\n"
               "        key: \"{{ hmailserver_rpm_key_url }}\"\n\n  always:\n", "more than once")
        broken("Copr's key given to rpm_key by its short name", os.path.join(TASKS, "install-copr.yml"),
               "# No gpgkey: the key is imported already",
               "- name: Import it again\n  rpm_key:\n    key: \"{{ hmailserver_copr_base }}/pubkey.gpg\"\n\n"
               "# No gpgkey: the key is imported already", "rpm_key itself")
        broken("the Build Service's key given to rpm --import", os.path.join(TASKS, "install-obs.yml"),
               "# No gpgkey: the key is imported already",
               "- name: Import it again\n  ansible.builtin.command:\n    argv: [rpm, --import, \"{{ hmailserver_obs_base }}\"]\n"
               "  changed_when: true\n\n# No gpgkey: the key is imported already", "rpm --import")
        broken("Copr's key included with keywords applied", os.path.join(TASKS, "install-copr.yml"),
               "  ansible.builtin.include_tasks: rpm-key.yml\n",
               "  ansible.builtin.include_tasks:\n    file: rpm-key.yml\n    apply:\n      ignore_errors: true\n", "is ignored")
        broken("the PPA's key check's failure ignored", os.path.join(TASKS, "install-ppa.yml"),
               "    - name: Refuse a key that is not the PPA's\n",
               "    - name: Refuse a key that is not the PPA's\n      ignore_errors: true\n", "is ignored")
        broken("the .deb's download given failed_when", os.path.join(TASKS, "install-deb.yml"),
               "    - name: Download the .deb and hold it to its SHA-256\n",
               "    - name: Download the .deb and hold it to its SHA-256\n      failed_when: false\n", "skip it")
        broken("the .deb's download's failure ignored", os.path.join(TASKS, "install-deb.yml"),
               "    - name: Download the .deb and hold it to its SHA-256\n",
               "    - name: Download the .deb and hold it to its SHA-256\n      ignore_errors: true\n", "is ignored")
        broken("the data directory's check taking a line feed", os.path.join(TASKS, "configure.yml"),
               "search('(?m)^Data directory:[ ]+\\S')", "search('(?m)^Data directory:[ ]+[^ ]')", "does not hold")
        broken("the refused settings' check skipped", os.path.join(TASKS, "configure.yml"),
               "  when: hmailserver_setting.results | default([]) | rejectattr('rc', 'eq', 0) | list | length > 0\n",
               "  when: false\n", "does not hold")
        broken("an account's password logged", os.path.join(TASKS, "objects.yml"),
               "  when: item.status == 404\n  changed_when: true\n  no_log: true\n",
               "  when: item.status == 404\n  changed_when: true\n", "does not hold")
        broken("a pin that is no SHA-256", RELEASE_YML, pins["debian13_amd64"], "sha256-of-nothing", "is not a SHA-256")
        broken("a pin missing", RELEASE_YML, "  debian12_arm64: ", "  debian11_arm64: ", "pins")
        broken("a fingerprint cut short", MAIN_VARS, "hmailserver_copr_fingerprint: A4", "hmailserver_copr_fingerprint: ",
               "40 hex digits")
        broken("the PPA's key fetched by another fingerprint", MAIN_VARS, "search=0xF1701CED", "search=0x0000",
               "fetched by the fingerprint")
        broken("a default administrator password", DEFAULTS, "hmailserver_admin_password_update:",
               "hmailserver_admin_password: changeme\nhmailserver_admin_password_update:", "gives hmailserver_admin_password")
        broken("an address on GitHub", os.path.join("roles", "hmailserver", "tasks", "main.yml"), "---\n",
               "---\n# https://github.com/example/example\n", "names GitHub")
        # The keywords a held task may carry, the modules' arguments, the names the
        # checks read, the PPA's source and the collection's changelog.
        copr = os.path.join(TASKS, "install-copr.yml")
        obs = os.path.join(TASKS, "install-obs.yml")
        ppa = os.path.join(TASKS, "install-ppa.yml")
        deb = os.path.join(TASKS, "install-deb.yml")
        configure = os.path.join(TASKS, "configure.yml")
        key_check = "    - name: Refuse a file that is not the channel's key alone\n"
        export_check = "    - name: Refuse an export that is not the channel's key alone\n"
        carry = "does not let it carry"
        for what, line in (("tagged never", "tags: [never]"), ("given loop: []", "loop: []"),
                           ("given with_items: []", "with_items: []"), ("given until", "until: true"),
                           ("given check_mode", "check_mode: true"), ("delegated", "delegate_to: localhost")):
            broken("the key file's check %s" % what, rpm_key, key_check, key_check + "      %s\n" % line, carry)
        broken("both of the key's checks tagged never", rpm_key, key_check, key_check + "      tags: [never]\n", carry,
               also=((export_check, export_check + "      tags: [never]\n"),))
        broken("the key file's check given a keyword under a quoted name", rpm_key, key_check,
               key_check + "      \"tags\": [never]\n", carry)
        broken("the key file's check given a line that is no keyword", rpm_key, key_check,
               key_check + "      <<: {tags: [never]}\n", carry)
        broken("the key's block skipped by a when", rpm_key, "- name: Fetch, check and import the key\n",
               "- name: Fetch, check and import the key\n  when: false\n", "skip it")
        broken("the key's block tagged never", rpm_key, "- name: Fetch, check and import the key\n",
               "- name: Fetch, check and import the key\n  tags: never\n", carry)
        broken("Copr's include of the key's checks tagged never", copr,
               "- name: Import Copr's key for the project, held to its fingerprint\n",
               "- name: Import Copr's key for the project, held to its fingerprint\n  tags: [never]\n", carry)
        broken("the Build Service's include of the key's checks given a further variable", obs,
               "    hmailserver_rpm_key_fingerprint: \"{{ hmailserver_obs_fingerprint }}\"\n",
               "    hmailserver_rpm_key_fingerprint: \"{{ hmailserver_obs_fingerprint }}\"\n    hmailserver_x: y\n",
               "does not hold")
        broken("Copr's install given disable_gpg_check", copr,
               "    state: \"{{ hmailserver_package_state if hmailserver_version == '' else 'present' }}\"\n",
               "    state: \"{{ hmailserver_package_state if hmailserver_version == '' else 'present' }}\"\n"
               "    disable_gpg_check: true\n", "may not be given")
        broken("the Build Service's install given disable_gpg_check", obs, "    update_cache: true\n",
               "    update_cache: true\n    disable_gpg_check: true\n", "verification is switched off")
        broken("the Build Service's install given extra_args", obs, "    update_cache: true\n",
               "    update_cache: true\n    extra_args: --quiet\n", "may not be given")
        broken("zypper told --no-gpg-checks on a command line", obs, "- name: Ask rpm for the version installed\n",
               "- name: Refresh\n  ansible.builtin.command:\n    argv: [zypper, --no-gpg-checks, refresh]\n"
               "  changed_when: false\n\n- name: Ask rpm for the version installed\n", "verification is switched off")
        ppa_install = "    cache_valid_time: \"{{ 0 if hmailserver_ppa_source is changed else 3600 }}\"\n"
        broken("the PPA's install given allow_unauthenticated", ppa, ppa_install,
               ppa_install + "    allow_unauthenticated: true\n", "verification is switched off")
        broken("the PPA's install given force, which implies it", ppa, ppa_install, ppa_install + "    force: true\n",
               "may not be given")
        broken("the PPA's source trusted", PPA_TEMPLATE, "Signed-By: {{ hmailserver_ppa_keyring }}\n",
               "Signed-By: {{ hmailserver_ppa_keyring }}\nTrusted: yes\n", "is not the PPA's source as held")
        broken("the PPA's source without Signed-By", PPA_TEMPLATE, "Signed-By: {{ hmailserver_ppa_keyring }}\n", "",
               "is not the PPA's source as held")
        broken("the PPA's source given a line by Jinja", PPA_TEMPLATE, "Components: main\n",
               "Components: main{{ '\\n' ~ 'Allow' ~ '-Insecure: yes' }}\n", "is not the PPA's source as held")
        broken("the PPA's source written to another file", ppa, "    dest: /etc/apt/sources.list.d/hmailserver-ppa.sources\n",
               "    dest: /etc/apt/sources.list.d/other.sources\n", "does not hold")
        broken("the PPA's key check tagged never", ppa, "    - name: Refuse a key that is not the PPA's\n",
               "    - name: Refuse a key that is not the PPA's\n      tags: [never]\n", carry)
        broken("the PPA's key fetched only when it is never", ppa, "    hmailserver_ppa_key_present.rc != 0 or\n",
               "    false and hmailserver_ppa_key_present.rc != 0 or\n", "does not hold")
        broken("the PPA's source written by apt_repository", ppa, "- name: Install hmailserver from the PPA\n",
               "- name: Name it again\n  ansible.builtin.apt_repository:\n    repo: ppa:hmailserver/hmailserver\n\n"
               "- name: Install hmailserver from the PPA\n", "written by a module of its own")
        deb_pin = "- name: Refuse a .deb whose SHA-256 is not pinned\n"
        broken("the .deb's pin check given loop: []", deb, deb_pin, deb_pin + "  loop: []\n", carry)
        broken("the .deb's pin check tagged never", deb, deb_pin, deb_pin + "  tags: [never]\n", carry)
        broken("the .deb's download and install tagged never", deb, "- name: Download and install the .deb\n",
               "- name: Download and install the .deb\n  tags: [never]\n", carry)
        broken("the .deb fetched without checking its host", deb,
               "        checksum: \"sha256:{{ hmailserver_deb_checksum }}\"\n",
               "        checksum: \"sha256:{{ hmailserver_deb_checksum }}\"\n        validate_certs: false\n",
               "may not be given")
        broken("a setting's value in the task's environment, by the setting's name", configure,
               "  register: hmailserver_setting\n",
               "  register: hmailserver_setting\n  environment:\n    HM_VALUE: \"{{ hmailserver_settings[item.key] }}\"\n",
               "outside its standard input")
        broken("a setting's value in the task's vars, by vars[...]", configure,
               "    # The [Settings] section as it was, to the next section's header.\n",
               "    hmailserver_x: \"{{ vars['hmailserver_' ~ 'settings'][item.key] }}\"\n"
               "    # The [Settings] section as it was, to the next section's header.\n", "outside its standard input")
        broken("the settings' loop item renamed", configure, "    label: \"{{ item.key }}\"\n",
               "    label: \"{{ item.key }}\"\n    loop_var: setting\n", "does not hold")
        broken("the gpg read given args", rpm_key, "      register: hmailserver_rpm_key_read\n",
               "      args:\n        chdir: /\n      register: hmailserver_rpm_key_read\n", "by args or action")
        broken("the refused settings' check tagged never", configure,
               "  when: hmailserver_setting.results | default([]) | rejectattr('rc', 'eq', 0) | list | length > 0\n",
               "  when: hmailserver_setting.results | default([]) | rejectattr('rc', 'eq', 0) | list | length > 0\n"
               "  tags: [never]\n", carry)
        broken("the configuration's check tagged never", configure,
               "- name: Refuse a configuration the service does not read as written\n",
               "- name: Refuse a configuration the service does not read as written\n  tags: [never]\n", carry)
        broken("the configuration's check taking any database type", configure,
               "\"{{ 3 if hmailserver_database_type == 'PostgreSQL' else 1 }}\"", "\"[0-9]+\"", "does not hold")
        broken("the database's password task given no_log twice", configure,
               "  no_log: true\n  notify: Restart hmailserver\n\n- name: Read the configuration, to see",
               "  no_log: true\n  no_log: false\n  notify: Restart hmailserver\n\n- name: Read the configuration, to see",
               "no_log twice")
        broken("gpg's reading set again by a set_fact", rpm_key, key_check,
               "    - name: Say what gpg read\n      ansible.builtin.set_fact:\n        hmailserver_rpm_key_read:\n"
               "          stdout: \"pub:\"\n\n" + key_check, "hmailserver_rpm_key_read 2 times")
        broken("gpg's reading registered by another task too", rpm_key, "      register: hmailserver_rpm_key_exported\n",
               "      register: hmailserver_rpm_key_read\n", "hmailserver_rpm_key_read 2 times")
        broken("the PPA's fingerprint set by a set_fact", ppa, "- name: Install what the key's check needs\n",
               "- name: Choose a key\n  ansible.builtin.set_fact:\n    hmailserver_ppa_fingerprint: \"0000\"\n\n"
               "- name: Install what the key's check needs\n", "hmailserver_ppa_fingerprint once")
        broken("the release's pins set by a set_fact", deb, "- name: Find the .deb's SHA-256\n",
               "- name: Pin another\n  ansible.builtin.set_fact:\n    hmailserver_release_deb_sha256={{ {} }}\n\n"
               "- name: Find the .deb's SHA-256\n", "hmailserver_release_deb_sha256 once")
        broken("a package given its arguments on the module's line", rpm_key,
               "  ansible.builtin.package:\n    name: \"{{ 'gpg2' if ansible_facts['os_family'] == 'Suse' else 'gnupg2' }}\"\n"
               "    state: present\n", "  ansible.builtin.package: name=gnupg2 state=present\n", "on the module's own line")
        broken("Copr's repository given gpgcheck again", copr, "    gpgcheck: true\n",
               "    gpgcheck: true\n    gpgcheck: false\n", "gpgcheck twice")
        broken("the Build Service's repository without its host checked", obs, "    repo_gpgcheck: true\n",
               "    repo_gpgcheck: true\n    sslverify: false\n", "may not be given")
        broken("a CHANGELOG.md without a heading for galaxy.yml's version", "CHANGELOG.md", "## %s\n" % version,
               "## 0.0.1\n", "has no heading")

        sums = "\n".join("%s  hmailserver_9.9.9-1~%s.deb" % (("%x" % i) * 64, target) for i, target in
                         enumerate(TARGETS, start=1)) + "\n%s  hmailserver-9.9.9.tar.gz\n" % ("f" * 64)
        got = pins_from_sums(sums, "9.9.9")
        expect("the pins read from a release's SHA256SUMS", got == {t: ("%x" % i) * 64 for i, t in enumerate(TARGETS, 1)})
        refused("a SHA256SUMS without one of the .deb files", lambda: pins_from_sums(
            sums.replace("debian12_arm64", "debian11_arm64"), "9.9.9"), "does not list hmailserver_9.9.9-1~debian12_arm64.deb")
        refused("a SHA256SUMS that lists a file twice", lambda: pins_from_sums(sums + sums.splitlines()[0] + "\n", "9.9.9"),
                "twice")

        calls = []
        tarball, built_version, built_pins, digest = build(os.path.join(work, "dist"), "9.9.9", got, run=fake_build(calls))
        expect("built for a release: galaxy.yml and release.yml rendered, read back from the tarball",
               built_version == "9.9.9" and built_pins == got and os.path.basename(tarball) == tarball_name("9.9.9"))
        expect("  ... by ansible-galaxy collection build over the staged copy",
               calls and calls[0][:4] == ["ansible-galaxy", "collection", "build", "--output-path"])
        with tarfile.open(tarball) as archive:
            expect("  ... with the repository's LICENSE in it",
                   archive.extractfile("LICENSE").read().decode("utf-8") == read(os.path.join(ROOT, "LICENSE")))
        refused("a tarball whose pins are not the ones it was built with",
                lambda: inspect(tarball, "9.9.9", dict(got, debian13_amd64="0" * 64)), "pins")
        refused("a tarball of another version", lambda: inspect(tarball, "9.9.8", got), "version")

        def failed_build(command, capture_output=True, text=True, env=None):
            return Answer(1, "", "ERROR! the collection is broken")
        refused("ansible-galaxy failing", lambda: build(os.path.join(work, "dist2"), "9.9.9", got, run=failed_build),
                "collection build failed")

        # publish
        source = os.path.join(work, "galaxy-build")
        os.makedirs(os.path.join(source, "dist"))
        shutil.copy(tarball, os.path.join(source, "dist"))
        write(os.path.join(source, RECORD), json.dumps({"version": "9.9.9", "tarball": tarball_name("9.9.9"),
                                                        "sha256": digest, "pins": got}))
        said = []
        globals()["say"] = said.append
        expect("without GALAXY_API_KEY: 'not configured', exit 0, nothing read",
               publish(os.path.join(work, "none"), {}, http=None) == 0 and said[0].startswith("not configured"))
        globals()["say"] = lambda line="": None
        held = rf.release_version
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            environ = {"GALAXY_API_KEY": "galaxy-secret-token"}
            asked = []

            def galaxy_world(has):
                state = {"has": has}

                def http(url, data=None, headers=None, method=None, timeout=120, fix=None):
                    asked.append(url)
                    return (200 if state["has"] else 404), {}, b"{}"

                def run(command, capture_output=True, text=True, env=None):
                    calls.append((command, env))
                    state["has"] = True
                    return Answer(0, "Collection has been published to the Galaxy server release_galaxy", "")
                return http, run
            del calls[:]
            http, run = galaxy_world(True)
            expect("a version Galaxy has already: nothing sent, exit 0",
                   publish(source, environ, http=http, run=run) == 0 and calls == [])
            http, run = galaxy_world(False)
            expect("a version Galaxy has not: ansible-galaxy collection publish of the tarball, exit 0",
                   publish(source, environ, http=http, run=run) == 0 and len(calls) == 1
                   and calls[0][0][:3] == ["ansible-galaxy", "collection", "publish"]
                   and calls[0][0][-1].endswith(tarball_name("9.9.9")))
            expect("  ... the token in its environment, as the server list names it, never on its command line",
                   calls and "galaxy-secret-token" not in " ".join(calls[0][0])
                   and calls[0][1].get("ANSIBLE_GALAXY_SERVER_RELEASE_GALAXY_TOKEN") == "galaxy-secret-token"
                   and calls[0][1].get("ANSIBLE_GALAXY_SERVER_LIST") == "release_galaxy"
                   and calls[0][1].get("ANSIBLE_GALAXY_SERVER_RELEASE_GALAXY_URL") == GALAXY_API
                   and "GALAXY_API_KEY" not in calls[0][1])
            expect("  ... Galaxy asked at its v3 address for the version",
                   asked and asked[-1] == GALAXY_API + "v3/plugin/ansible/content/published/collections/index/"
                   "progressiverobot/hmailserver/versions/9.9.9/")

            def leaking(command, capture_output=True, text=True, env=None):
                return Answer(1, "", "refused for galaxy-secret-token")
            http, _ = galaxy_world(False)
            try:
                publish(source, environ, http=http, run=leaking)
                expect("a failed publish: refused", False)
            except Refused as error:
                expect("a failed publish: refused, without the token in what it says",
                       "galaxy-secret-token" not in str(error) and "(the token)" in str(error))
            rf.release_version = lambda environ, fix: ("9.9.10", "the update feed")
            refused("the release moved on since galaxy-build", lambda: publish(source, environ, http=http, run=run),
                    "the release is now 9.9.10")
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            with open(os.path.join(source, "dist", tarball_name("9.9.9")), "ab") as f:
                f.write(b"\0")
            del calls[:]
            refused("a tarball that is not the one galaxy-build recorded", lambda: publish(source, environ, http=http, run=run),
                    "is not the tarball galaxy-build recorded")
            expect("  ... nothing was sent", calls == [])
        finally:
            rf.release_version = held

        # release-build, with the release verified as release-files.py verifies it, from a
        # tree whose collection's CHANGELOG.md has a heading for the release.
        root9 = os.path.join(work, "root-9.9.9")
        os.makedirs(os.path.join(root9, os.path.dirname(COLLECTION)))
        shutil.copytree(os.path.join(ROOT, COLLECTION), os.path.join(root9, COLLECTION),
                        ignore=shutil.ignore_patterns(*SKIP))
        shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(root9, "LICENSE"))
        changelog9 = os.path.join(root9, COLLECTION, CHANGELOG)
        write(changelog9, read(changelog9) + "\n## 9.9.9\n\nBuilt for the self-test.\n")
        held = (rf.release_version, rf.tool_path, rf.verified_files, rf.write_record)
        try:
            rf.release_version = lambda environ, fix: ("9.9.9", "HM_CHANNEL_VERSION")
            rf.tool_path = lambda tool, work, environ=None: "cosign"
            sums_path = os.path.join(work, SUMS % "9.9.9")
            write(sums_path, sums)

            def unverified(*args, **kwargs):
                raise Refused("hmailserver-linux-9.9.9-SHA256SUMS.txt does not verify as signed by this project's pipeline")
            rf.verified_files = unverified
            del calls[:]
            refused("release-build for a version the collection's CHANGELOG.md has no heading for: refused before"
                    " anything is fetched", lambda: release_build(os.path.join(work, "rb0"), {}, run=fake_build(calls)),
                    "has no heading")
            expect("  ... nothing built", calls == [])
            refused("release-build, the sums not verifying: refused before anything is built",
                    lambda: release_build(os.path.join(work, "rb1"), {}, root=root9, run=fake_build(calls)),
                    "does not verify")
            expect("  ... nothing built", calls == [])
            rf.verified_files = lambda version, names, into, cosign, fix=None, run=None: {names[0]: {
                "path": sums_path, "sha256": rf.sha256_file(sums_path), "size": 1,
                "identity": rf.identities("9.9.9")[1], "repository_id": "86730559"}}
            rf.write_record = lambda *args, **kwargs: None
            out = os.path.join(work, "rb2")
            code = release_build(out, {}, root=root9, run=fake_build(calls))
            with open(os.path.join(out, RECORD), encoding="utf-8") as f:
                record = json.load(f)
            expect("release-build: the collection built for the release with the verified sums' pins",
                   code == 0 and record["version"] == "9.9.9" and record["pins"] == got
                   and record["sha256"] == rf.sha256_file(os.path.join(out, "dist", tarball_name("9.9.9"))))
        finally:
            rf.release_version, rf.tool_path, rf.verified_files, rf.write_record = held

        if shutil.which("ansible-galaxy"):
            tarball, built_version, _, _ = build(os.path.join(work, "real"))
            expect("the tree's collection built by the real ansible-galaxy and read back", built_version == version)
        else:
            print("skip  the real ansible-galaxy build: ansible-galaxy is not installed here")
    finally:
        globals()["say"] = held_say
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("command", nargs="?", choices=["check", "build", "release-build", "publish"])
    parser.add_argument("--out", default="galaxy-build")
    parser.add_argument("--from", dest="source", default="galaxy-build")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        if arguments.command == "check":
            return check_command()
        if arguments.command == "build":
            build(os.path.abspath(arguments.out))
            return 0
        if arguments.command == "release-build":
            return release_build(os.path.abspath(arguments.out))
        if arguments.command == "publish":
            return publish(os.path.abspath(arguments.source))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1
    parser.print_usage()
    return 2


if __name__ == "__main__":
    sys.exit(main())
