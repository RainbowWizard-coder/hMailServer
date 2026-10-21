hMailServer: this fork and the original
=======================================

Two projects are called hMailServer. This page says which is which, what each has
published, and what differs between them, for someone choosing one. What it says of
the original is read from the original's own release pages and repository on
21 October 2026; the links lead there, and that is where to check anything that has
changed since.

The two projects
----------------

**This project is Progressive Robot's fork of hMailServer.** Progressive Robot Ltd
forked the original in 2026 and released 6.0.0 on 12 June 2026; the latest release is
6.3.7, of 17 October 2026. It is maintained by Christopher Holloway and Progressive
Robot Ltd, and its versions are numbered 6.x.

* Home: <https://gitlab.com/hmailserver/hmailserver>, where it is worked on, with the
  releases at <https://gitlab.com/hmailserver/hmailserver/-/releases>.
* A copy: <https://github.com/RainbowWizard-coder/hMailServer>, GitLab's mirror, with
  the releases and Discussions.

**The original is hMailServer by Martin Knafve**, at
<https://github.com/hmailserver/hmailserver>, with its website at
<https://www.hmailserver.com>. Its versions are numbered 5.x and earlier. Its releases
page keeps the builds from 4.1.1 to 5.6.9 build 2607 as "Legacy releases", which it
says "are no longer supported and are kept for reference only", and lists these:

| Release | Published | What its notes say |
|---|---|---|
| [5.7.0, build 3013](https://github.com/hmailserver/hmailserver/releases/tag/hMailServer-5.7.0-b3013) | 28 September 2026 | DMARC checking of incoming mail; passwords stored with Argon2id or PBKDF2; bounces that thread with the original message; a list of fixes; new installations run the service as Network Service, with data and logs in `C:\ProgramData\hMailServer`. Its pre-release builds [2788](https://github.com/hmailserver/hmailserver/releases/tag/hMailServer-5.7.0-b2788) (of 22 August 2026) and [2843](https://github.com/hmailserver/hmailserver/releases/tag/hMailServer-5.7.0-b2843) (of 29 August 2026), put on the releases page the same day as 3013, brought TLS 1.3, special-use mailboxes (RFC 6154), DKIM improvements and MariaDB Connector/C, and made it 64-bit only |
| [5.7.1, build 3116](https://github.com/hmailserver/hmailserver/releases/tag/hMailServer-5.7.1-b3116) | 29 September 2026 | A security release: a script injection in event scripts fixed |
| [5.7.2, build 3124](https://github.com/hmailserver/hmailserver/releases/tag/hMailServer-5.7.2-b3124) | 29 September 2026, as a pre-release | OpenSSL 3.5.9 |

Its repository's latest commits, read the same day, are of 11 October 2026, and the
`VERSION` file of its master branch reads 5.8.0.

This fork is not made by Martin Knafve or by the original project, and it is not a
successor to it: the two are developed separately.

Credit
------

hMailServer is Martin Knafve's work, with that of the people who contributed to it
([the original's contributors](https://github.com/hmailserver/hmailserver/graphs/contributors)).
This fork is built on that work. The licence terms this fork's installer shows end
with the sentence "This software is a fork of hMailServer, originally created by Martin Knafve,
whose copyright in the original work remains." The fork also reads what the original
publishes and takes up its fixes in the code the two share, and says where it does:
several of 6.3.7's security fixes were found by the original project (the
[CHANGELOG](../../CHANGELOG.md)'s 6.3.7 section, "Found by upstream hMailServer").

The differences at a glance
---------------------------

| | This fork | The original |
|---|---|---|
| Versions | 6.0.0 to 6.3.7. The 6.3 line has security fixes until 30 September 2027 ([SECURITY.md](../../SECURITY.md#supported-versions)) | 5.7.1 its latest release, 5.7.2 a pre-release |
| Windows | 64-bit, from Windows 10 1607 and Windows Server 2016; an ARM64 installer as a preview that has never run on ARM64 hardware ([WindowsOnArm.md](WindowsOnArm.md)) | 64-bit from 5.7.0; its installer runs from Windows Vista SP2 |
| Linux | Since 6.3.0. The latest release has a `.deb` and an `.rpm` for Debian 12 and 13, Ubuntu 24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed, and an AppImage, each for x86-64 and AArch64 ([PackagesDeb.md](PackagesDeb.md), [PackagesRpm.md](PackagesRpm.md), [PackagesAppImage.md](PackagesAppImage.md)) | Its releases are Windows installers |
| Containers and Kubernetes | A signed image, `registry.gitlab.com/hmailserver/hmailserver`, for linux/amd64 and linux/arm64, and a Helm chart ([ContainerImage.md](ContainerImage.md)) | Its releases are Windows installers |
| Database schema | 6108 in 6.3.7 | 5711 in 5.7.0 build 3013, 5.7.1 and 5.7.2 |
| Licence | AGPL-3.0-or-later | AGPLv3, by its installer's licence terms (below) |
| Issues | GitLab | GitHub |

What the fork added
-------------------

The major features, each with the document that has the detail. README.md's
[Capabilities](../../README.md#capabilities) is the whole list, and the
[CHANGELOG](../../CHANGELOG.md) says what changed in each release. Some of them the
original's 5.7 has now as well - DMARC checking, Argon2id and PBKDF2 password storage,
TLS 1.3 and special-use mailboxes among them, by its release notes above.

* **Linux, AArch64, the container image and the Helm chart**, above.
* **Administration from a browser and from scripts**: a REST API, the Control Deck
  that runs on it, and the command-line clients `hmctl` and the `HmailServer` PowerShell
  module, beside a Control Panel rebuilt on .NET 10 ([README.md](../../README.md#administration)).
* **A webmail and self-service portal**, with calendars, tasks and contacts in it and
  over **CalDAV and CardDAV** to phones and desktop clients ([Calendar.md](Calendar.md),
  [Contacts.md](Contacts.md)).
* **Sieve** filtering, with ManageSieve, beside the rules the original has.
* **Transport security**: MTA-STS, DANE with DNSSEC validation, TLS-RPT reports and
  ACME certificates ([README.md](../../README.md#transport-security)).
* **Sender authentication**: DMARC with its reports, ARC sealing, SRS and BATV
  ([DmarcReports.md](DmarcReports.md)).
* **Sign-in**: SCRAM-SHA-256, OAuth 2.0 and OpenID Connect, passkeys and single sign-on,
  LDAP and SCIM provisioning, and secrets sealed at rest
  ([PasskeysAndSingleSignOn.md](PasskeysAndSingleSignOn.md), [Scim.md](Scim.md),
  [SecretsAtRest.md](SecretsAtRest.md)).
* **Migration jobs** that bring mailboxes in from another IMAP server, Google Workspace,
  Microsoft 365 or Outlook's data files ([Migration.md](Migration.md)).
* **Operations**: Prometheus metrics, a live update from the release feed, and the
  settings kept in the database with setting profiles ([UpdateFeed.md](UpdateFeed.md),
  [Profiles.md](Profiles.md)).

Moving an installation between them
-----------------------------------

The two share the database's upgrade chain up to schema 5708 and part company there.
[Upgrading.md](Upgrading.md#where-you-can-upgrade-from) has the detail.

* **From the original up to 5.6 to this fork: in place.** Run this fork's installer
  over the installation: it upgrades the database and keeps the configuration and the
  mail. Every 5.6 release requires schema 5601, which this fork's upgrade chain takes
  on to its own.
* **From the original's 5.7 releases to this fork: not in place.** 5.7.0 build 3013,
  5.7.1 and 5.7.2 require schema 5711, and the pre-release builds 2788 and 2843 schema
  5709: steps of the original's own, which this fork's chain does not have. Do not run this
  fork's installer over such an installation: its database step stops without changing
  the database, but only after the programs have been replaced, and this fork's server
  will not run on that database. Move the mail instead: this fork installed on another
  machine, the domains and accounts made there, and each mailbox copied across over
  IMAP by a migration ([Migration.md](Migration.md#from-the-original-hmailserver)).
* **From this fork back to the original: no.** The original's server refuses a
  database newer than the one it requires ("The database is too new for this version
  of hMailServer"), and neither project downgrades a database. The way back from an
  upgrade is the backup taken before it ([Upgrading.md](Upgrading.md#rolling-back)).
* **A Windows machine holds one of them.** Both installers register under one setup
  id, `hMailServer_is1`, so each one's setup finds an installation of the other as its
  own.

Where a package manager's plain name is already the original's, the fork's package
has another: on Chocolatey the id `hmailserver` is a package of the original, and
this fork's 6.3.7 was submitted as `hmailserver-progressiverobot`.

Security fixes the fork has published
-------------------------------------

The CVE identifiers assigned to vulnerabilities in this fork's releases, by GitLab,
the CVE Numbering Authority for projects on GitLab.com, as the CHANGELOG and the
records at [OSV](https://osv.dev) give them. Each record names this fork's affected
versions, and the platform where only one is affected. None of them is a statement
about the original's releases, for which the original's own release notes speak.

| Identifier | The flaw | Affected | Fixed in |
|---|---|---|---|
| CVE-2026-100741 | Script injection through the values passed to JScript event scripts | 6.0.0 to 6.3.3, Windows | 6.3.4 |
| CVE-2026-103010 | A heap buffer overflow in the legacy Blowfish decryption of stored secrets | 6.0.0 to 6.3.3, Windows | 6.3.4 |
| CVE-2026-103011 | A heap buffer overflow in the legacy Blowfish encryption | 6.0.0 to 6.3.5 | 6.3.6 |
| CVE-2026-103647 | The webmail served an attachment it had decrypted under the type the message named | 6.3.2 to 6.3.5 | 6.3.6 |
| CVE-2026-103649 | Network waits with no timeout | 6.3.0 to 6.3.5, Linux | 6.3.6 |
| CVE-2026-104658 | The Linux live update's apply step trusted the request the service account wrote | 6.3.4 and 6.3.5, Linux | 6.3.6 |
| CVE-2026-104659 | The REST listener's loopback port answered any `Host`, and wrong administrator passwords were not slowed down | 6.0.0 to 6.3.5 | 6.3.6 |
| CVE-2026-104660 | COM helpers that worked for a local user with no hMailServer credential | 6.0.0 to 6.3.5, Windows | 6.3.6 |
| CVE-2026-104704 | Outbound DANE did not require TLS to a host whose TLSA records it could not use | 6.0.0 to 6.3.5 | 6.3.6 |
| CVE-2026-107566 | Sieve `editheader`, and a rule's *Set header value*, could rewrite who a message was from | 6.0.0 to 6.3.6 | 6.3.7 |
| CVE-2026-107567 | Mail from outside that named a hosted domain was signed with that domain's DKIM key | 6.0.0 to 6.3.6 | 6.3.7 |
| CVE-2026-107568 | Sieve `notify`, and a rule's *Reply*, could send from an address the owner may not send as | 6.0.0 to 6.3.6 | 6.3.7 |
| CVE-2026-107569 | The rule loop limit could be lifted, so that generated mail could loop without bound | 6.0.0 to 6.3.6 | 6.3.7 |
| CVE-2026-107571 | Programs any user could replace, on a Windows server installed in a folder that inherits a writable parent's permissions, such as the root of a drive | 6.0.0 to 6.3.6, Windows | 6.3.7 |

[SECURITY.md](../../SECURITY.md) says how to report a vulnerability and which versions
receive fixes.

Licence
-------

This fork is licensed under the GNU Affero General Public License, version 3 or later
(AGPL-3.0-or-later): [LICENSE](../../LICENSE), and the SPDX identifier in each of its
source files. The original's repository has no licence file at its root; the licence
terms its installer shows (`hmailserver/installation/License.rtf`) say that "The
hMailServer source code is licensed under AGPLv3, with exception for 3'rd party
libraries licensed otherwise", and give the licence's text. So both are under version
3 of the GNU AGPL, and this fork marks its files "or later". Third-party components
keep their own licences, which [Licenses](Licenses/) lists. This page describes the
licences; it is not legal advice.

Support, and where each takes issues
------------------------------------

**This fork**: a defect or a question is an issue on
[GitLab](https://gitlab.com/hmailserver/hmailserver/-/issues), with the Bug or the
Question template; the copy on GitHub has no Issues. Questions may also go to
[GitHub Discussions](https://github.com/RainbowWizard-coder/hMailServer/discussions)
or the forum at <https://www.hmailserver.co.uk/>, and anyone without a GitLab account
can write to the project's Service Desk address. A vulnerability is reported privately,
as [SECURITY.md](../../SECURITY.md) says. [SUPPORT.md](../../SUPPORT.md) has the rest.

**The original**: issues on
[GitHub](https://github.com/hmailserver/hmailserver/issues), and its website,
<https://www.hmailserver.com>, to which its README sends readers for everything but
building it.

A question about the original is best asked of the original, and one about this fork
here: a report sent to the wrong project describes a server that project did not ship.
