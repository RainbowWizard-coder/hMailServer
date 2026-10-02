# Changelog

This file records what changed in each release of hMailServer that this project has published, newest first,
and what is in the tree after the latest release. It is a summary. The full notes of a release - the reasoning,
the tests and the verification - are on its GitLab release page,
`https://gitlab.com/hmailserver/hmailserver/-/releases/<tag>`, which every section below links to (the sections up
to 6.3.5 link the project's first path, `gitlab.com/Progressiverobot/hmailserver`, which GitLab redirects).

This project is a maintained fork of hMailServer 5.7 by Martin Knafve. Its own releases begin at 6.0.0; the
`hMailServer-5.7.0-bNNNN` tags in the repository are upstream builds, not releases of this project.

**The latest release is 6.3.6, published on 1 October 2026.** Anything that arrived after it is in the tree
after 6.3.6 and not yet released. It is listed under [Unreleased](#unreleased---in-the-tree-after-636-not-yet-released)
and will carry a version number when a release is cut.

## Where the releases are

The project's home is <https://gitlab.com/hmailserver/hmailserver>, in the group `hmailserver` since 29 September
2026 (its first path on GitLab, `Progressiverobot/hmailserver`, redirects there). Its releases were published on
GitHub until GitHub suspended the account on 18 September 2026, and every `github.com/Progressiverobot` address in
older text now answers 404. Every release up to 6.3.3 has been re-created on GitLab from the GitHub original, under
its original date and with its files byte for byte, but for 6.2.0 and 6.0.0-B4, whose files were not preserved:
<https://gitlab.com/hmailserver/hmailserver/-/releases>, with the latest always at
<https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest>. A tag with no release is at
`https://gitlab.com/hmailserver/hmailserver/-/tags/<tag>`.

One release is not a version of the server and has no section here:
[`build-inputs-1`](https://gitlab.com/hmailserver/hmailserver/-/tags/build-inputs-1), of 11 September 2026,
holds the three third-party files the Windows installer build downloads instead of keeping them in git - the
7-Zip 19.00 console, the MariaDB Connector/C 3.4.9 client and the SQL Server Compact 4.0 SP1 x64 runtime. Each is
recorded by SHA-256 in `hmailserver/docs/third-party-binaries.json` and fetched by
`build/get-installer-binaries.ps1`. Since 25 September 2026 the 7-Zip console is 26.03, fetched from 7-zip.org itself, and
the release's 7-Zip 19.00 is no longer used.

## How to read a section

- **Dates** are UTC. Where the release's own publication time was not saved, the date is the tag's, and the
  heading says so.
- **Schema** is the database version the release requires: `REQUIRED_DB_VERSION` in
  `hmailserver/source/Server/Common/Application/Constants.h` at the tag. The installer applies the steps with
  DBUpdater on Windows; from 6.3.0, `hmailserver --upgrade-database` does it on Linux. The upgrade is one-way, so
  take a backup of the database and of the data directory before installing.
- **Pre-releases** (the 6.0.0 B builds, 6.2.22-pre1 to pre6 and 6.2.23 Alpha 1 and 2) are listed where they fall
  and marked as such. Three tags have no release and are listed in one line each: 6.0.0-B3, for which no
  release is known, and 6.2.22-pre4 and pre5, whose releases were deleted.

## Issue and pull-request numbers

Every issue, pull-request and discussion number written before 24 September 2026 is GitHub's, from the project's
former home, and this file writes it in words: "GitHub issue 263", "GitHub PR 12", "GitHub discussion 18". GitLab
numbers its own issues from 1 again and links a bare `#` and a number to its own issue of that number, so no
number is written that way here. Upstream hMailServer's numbers are written the same way, as "upstream PR 552" or
"upstream issue N". A person's handle on either forge is written in a code span, so that nobody is notified.

## How this file is kept

By hand. It is not generated: the project does not use GitLab's `Changelog:` commit trailers. The Unreleased
section is updated when a batch lands on `master`, never by the commits of individual roadmap rows, which would
make every fold of parallel work conflict on this file. A released version's section is written once, before
its tag, in the commit the tag points at: from 6.3.5 the tag's pipeline makes the GitLab release's notes from
that section as the tag's own tree has it (`build/ci/release-chain.py`, `release-publish`), and refuses a version
without one, so the section cannot be added afterwards. GitLab CI's `checkers` job runs
`python build/add-license-headers.py --check` on every push; after editing, run it too and read the rendered page
for a stray `#` number or `@` name.

---

## Unreleased - in the tree after 6.3.6, not yet released

---

## [6.3.6](https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.6) - 2026-10-01

Everything that landed on `master` after the `v6.3.5` tag, from 29 September 2026 ([compare `v6.3.5` with
`v6.3.6`](https://gitlab.com/hmailserver/hmailserver/-/compare/v6.3.5...v6.3.6)). It is the last release for the
time being: after it the project makes security releases only, and the 6.3 line receives security fixes until 30
September 2027 (below). 6.3.6 fixes a security flaw and takes OpenSSL 4.0.3 into the Windows installers (both below),
fixes what 6.3.5's release and the first live updates from 6.3.4 found, replaces with the project's own code the last
code in the server whose licence a distribution would have had to settle, and carries every file the release pipeline
makes, the ARM64 Windows installer among them. The schema moves to **6108** in three steps: 6106, which changes no
column, gives every external account's *days to keep messages* the one meaning both fetchers now read, keeping each
account doing what it did or stopping it deleting (What changes on upgrade without a switch, item 21); 6107 finishes
the greylisting key schema 6072 began (item 22); and 6108 adds the never-ban list by reverse name and, for each SMTP
listener, what it does with `AUTH`, On for every listener an upgrade finds (item 24). The security notes come first,
then what changes on upgrade without a switch. The operator documents named below are in
`hmailserver/docs`, which has nine more of them than it had at 6.3.5.

**Releases pause after this one.** 6.3.6 is the last release for the time being, by the owner's decision of 28
September 2026, and no feature release follows it until the maintainers say otherwise in [SECURITY.md](SECURITY.md),
"While releases pause". A confirmed vulnerability still ships, in a release of its own that is 6.3.6 with fixes -
6.3.7, 6.3.8 and so on - on the targets SECURITY.md gives: a critical or high one as soon as its fix has passed the
gates, and none held past 90 days. Other defects are fixed on `master` and ship with the next security release, or
when releases resume. The 6.3 line receives security fixes until 30 September 2027; before that date the
maintainers write in SECURITY.md whether support continues, and if nothing is written, it ends then. There are
still no maintenance branches: an installation of 6.3.5, 6.2 or anything older is upgraded in place to the newest
release.

**What the release carries**: 60 files and 154 links (ContinuousIntegration.md, "What a release carries"). Beside
the x64 installer is `hMailServer-6.3.6-arm64.exe`, for Windows on ARM, Authenticode-signed like it. It is a
preview: the ARM64 build is cross-compiled on the project's x64 Windows runner and has never run on ARM64 hardware,
so read it as built to work ([WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md)). If you install it on an ARM64
machine, please say how it went, working or not, in [an issue](https://gitlab.com/hmailserver/hmailserver/-/issues):
the project has no ARM64 machine of its own. SQL Server Compact has no ARM64 build, so a new ARM64 installation is
set up against SQL Server or PostgreSQL, and a silent one is given its database on the command line. For Linux the
release carries the generic `.deb` and `.rpm` for x86-64 and, for the first time since 6.3.3, for AArch64 -
cross-compiled and tested under emulation, since no AArch64 machine has run them - and each Debian 12 and 13, Ubuntu
24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed release's own package for both
architectures. It also carries both AppImages, with the source of the libraries they carry and of those their
runtime links; the source tarball, `hmailserver-6.3.6.tar.gz`, reproducible and with nothing non-free in it, from
which the Debian source package and the RPM spec in the tree build the server as a distribution does
([SourceTarball.md](hmailserver/docs/SourceTarball.md), [SourcePackages.md](hmailserver/docs/SourcePackages.md));
and the Helm chart. Every file has a Sigstore bundle beside it, every compiled file and the tarball an SBOM, and
SLSA build provenance names every file by its SHA-256 ([SECURITY.md](SECURITY.md), "Verifying a release" and
"Build provenance"). The apt and dnf repositories and the other channels publish from `master` once their keys and
accounts exist; none is live as this release is made ([Channels.md](hmailserver/docs/Channels.md)).

**The first container image**: `registry.gitlab.com/hmailserver/hmailserver:6.3.6`, also tagged `6.3` and `latest`,
one index over linux/amd64 and linux/arm64, each platform's image built from this release's `.deb` of its
architecture. It is pushed from this release's own pipeline once the release is published, by a job started by
hand, signed with cosign as that pipeline, each platform's image with its SBOM attested to it; until then the
registry holds no image. In the container the update check names the image to pull and installs nothing
([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

**A Nix flake and Alpine Linux**: a Nix flake of the project's own at the repository's root, with a NixOS module
(`services.hmailserver`), and an APKBUILD and an OpenRC service for Alpine Linux, on whose musl the server now
builds and runs; both are built without the live update and name the command that updates them, and neither is in
a distribution's archive yet ([Nix.md](hmailserver/docs/Nix.md), [Alpine.md](hmailserver/docs/Alpine.md)).

**Signing, and who installs this release by the live update.** Every file is signed with Sigstore as this
release's own pipeline at the group's path,
`https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.6`, issuer `https://gitlab.com`, with the
project's id, 86730559, in the certificate: the first release signed there since the project moved into the group
`hmailserver` on 29 September 2026. Every server from 6.3.4 on has that signer compiled in and requires the id for
it, so a server on 6.3.4 or 6.3.5 installed from the x64 installer or the release's x86-64 `.deb` or `.rpm` is built
to install this release by its live update: from 6.3.5 with 6.3.5's own file as its rollback image, and from 6.3.4,
which carries no Sigstore bundle, without one. No live update had installed a release signed at the group's path
when these notes were written. The update is applied by the version already installed, so three things of 6.3.4
and 6.3.5 still hold for it:

- a Windows service running as `NT SERVICE\hMailServer` downloads and verifies the installer, then leaves the
  install to an administrator (`HM7463`), whose *Install update* on 6.3.4 and 6.3.5 cannot do it: install this
  release by hand, once, as the next paragraph says, and every release after it with one click;
- on Linux, a backup destination inside `/var/lib/hmailserver` fails the backup taken before an unattended apply
  (`HM5014`), and the apply is refused until the destination is moved or the release installed by hand
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "From 6.3.4 on, by the live update");
- a server whose `UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository` still name the path
  the project began at - set so that a server from 6.2.28 to 6.3.3 could install 6.3.5 - trusts that path alone and
  refuses this release. Clear the three first - in the Control Panel, with `DELETE /api/v1/settings/ini/<name>`, or
  on Linux with `hmailserver --set-setting <name>=` - and not in the file, which 6.3.4 and 6.3.5 put back.

A server from 6.2.28 to 6.3.3 trusts only the GitHub workflow that signed the releases up to 6.3.3, and is upgraded
to this release by hand, once, after the backup [Upgrading.md](hmailserver/docs/Upgrading.md) describes; so is a
server installed from an AArch64 package or an AppImage, whose last release was 6.3.3. One pointed at this
project's feed with `UpdateFeedUrl` downloads the installer at every check and refuses it, so keep
`UpdateAutoDownload` off there until then. The three settings that let such a server install 6.3.5 do not reach
6.3.6, and must never name the group's path ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "A server from
6.2.28 to 6.3.3"). No release before this one carried the ARM64 installer, so no ARM64 Windows server has a live
update to take it; from 6.3.6 one finds its installer by name, and an x64 server is never offered it. A server
installed from one of this release's per-distribution packages is offered the next release's generic package by its
live update, which apt refuses on Debian 12 and dnf on EL 9, so there it is upgraded with its package manager and
its own release's package of the next version, not by the live update
([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "The download links").

**One click installs an update where the service cannot.** A Windows service running as `NT SERVICE\hMailServer`,
the account a new installation has had since 6.3.4, is not an administrator and cannot run an installer, so up
to 6.3.5 *Install update* told the administrator to download the release and run it by hand. From 6.3.6 the
Control Panel on the server installs it: *Install update* on the Status page, one Windows prompt, and everything
else happens by itself - the installer is checked, the backup taken, the version to go back to fetched, the
server drained, the release installed, the service started, the previous version put back if it does not start,
and the Control Panel opened again, signed in, on the outcome. What runs elevated trusts nothing the service
wrote: `hMailServer.exe /InstallUpdate` checks that only administrators can change the installation, copies the
installer out of the service's `Updates` folder into `AdminUpdate`, which only administrators can change, and
checks the copy itself - the Authenticode signature with the publisher pinned to Progressive Robot Ltd and the
Microsoft chain it is issued under, revocation online, the version and the architecture, a release newer than
the running one, and the Sigstore bundle, in a child process with the administrator's rights taken away.
`SeDebugPrivilege`, which the stage needs to read a Control Panel of another user's and the Updater to end the
programs running from the installation folder, is switched on for that alone and off again before anything else
runs, the installer included. The Control Deck, the REST API and a Control Panel connected to another computer say
where to click. The service
never makes an administrator or SYSTEM run anything ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md),
"The live update"). On LocalSystem the live update is as it was.

A server on 6.3.4 or 6.3.5 under `NT SERVICE\hMailServer` still says to install this release by hand, since that
is compiled into it; nothing in the feed or the release can change what it does. Download
`hMailServer-6.3.6-x64.exe` from this release, check that `Get-AuthenticodeSignature` reports it `Valid` and
signed by `CN=Progressive Robot Ltd`, and run it as an administrator, keeping the service account on the
installer's *Service account* page. On a machine that has
not seen the file before, Windows SmartScreen may say it "prevented an unrecognized app from starting": it
judges a file by how many machines have run it as well as by its signature, and a new release is new to it.
*More info* names the publisher, Progressive Robot Ltd; with the signature checked, *Run anyway* installs it.
The one-click install never meets SmartScreen: the installer is not downloaded by a browser, and the Control
Panel's updater starts it itself.

### Security

The code of the legacy Blowfish scheme of stored secrets that the project inherited from the original hMailServer,
which this release replaces with its own (under Operations), had two heap buffer overflows. The one in its
encryption is in every version up to 6.3.5, and 6.3.6 fixes it. The one in its decryption is in every
version up to 6.3.3, and was fixed in 6.3.4, whose notes did not say so. Both were found by the project's own
review. The same release makes the helpers of COM `Utilities` that do work in the server answer only a caller that
has signed in, or an event script running in the service (What changes on upgrade, item 1). GitLab, the CVE
Numbering Authority for projects on GitLab.com, has assigned them CVE-2026-103011 (the encryption, fixed in 6.3.6)
and CVE-2026-103010 (the decryption, fixed in 6.3.4);
[work item #49](https://gitlab.com/hmailserver/hmailserver/-/work_items/49) gives the details from the day of the
release. Upgrade every server running 6.3.5 or earlier.

Calendar messages delivered by mail (iMIP) - a cancellation, an attendee's answer, an updated invitation - changed
the event stored in the recipient's calendar without the server checking who had sent them, in 6.3.4 and 6.3.5;
6.3.6 applies one only when the server verified that it came from the event's organiser, or from the answering
attendee or a delegate of theirs, and delivers anything else as an ordinary message; an invitation filed from a
sender it could not verify no longer keeps the organiser's own out of the calendar (What changes on upgrade,
item 16). A room or a piece of equipment took a DMARC pass for an outside organiser's domain as the organiser's
own, whichever sender at that domain had written the message, in 6.3.4 and 6.3.5; 6.3.6 believes that organiser
only when the message's From is the organiser's address. A room or a piece of equipment also took an invitation
an application sent through the send API, with a key allowed to send as the organiser's address, as the
organiser's own, in 6.3.4 and 6.3.5, where its documentation said such a message is not believed; in 6.3.6
neither a room nor a calendar takes a message sent through the send API as an account's own.

The Control Deck is now served under a script policy that lets its own script run and nothing else - the SHA-256 of
its one inline script, which the server takes from the page it serves - with Trusted Types, whose one policy refuses
markup the Deck never builds, and with X-Frame-Options and Cross-Origin-Opener-Policy, where 6.2.28 to 6.3.5 allowed
any inline script on the page and earlier versions sent it with no policy at all; a proxy in front of the server
must now pass the page through unchanged ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md), Behind
a reverse proxy).

The webmail's pages are now served with Trusted Types, whose one policy takes out of what a page puts into itself
the elements the page never builds and admits only the page's own two script addresses, and with X-Frame-Options and
Cross-Origin-Opener-Policy, none of which any earlier version sent.

An account's formatted signature is now given to the webmail only as the subset the webmail's own editor admits -
basic formatting, links to http(s) and mailto, inline images, a simple table, and nothing else - whatever is stored,
and the account holder's own save is stored that way. The webmail puts a signature on the screen as markup,
including in an administrator's support session as the account, and earlier versions cleaned it only in the browser
that saved it, so a signature written another way could carry script into the browser that later rendered it. The
signature an administrator sets in the Control Panel, over COM or over the account route is stored as it is written
and appended to the account's outgoing mail with its styles and hosted images, as before; a domain's signature,
which no browser renders as markup, is unchanged.

A message attachment is now served only under a media type from a list of those no browser runs, applies or renders
as a page - pictures, sound, video, PDF, text, the office formats and the archives - and as `application/octet-stream`
otherwise. Earlier versions kept any declared type that was not one of a few named ones, which left the
executable-JavaScript media-type aliases a browser also runs; the attachment already carried
`Content-Disposition: attachment`, `nosniff` and a sandbox policy.

The REST administration listener's plain loopback port now answers only a request whose `Host` it expects and refuses
any other with 421 before the request is authenticated, so a web page that points a name of its own at this machine
can no longer reach the console through a browser on it. It answers `localhost`, an IP address, the server's host
name and the host of each address configured as the one the webmail is reached at from outside. A reverse proxy that
forwards the browser's own `Host` needs that name to be one of these: upgrading behind such a proxy, set the
webmail's outside address (for example `confidential_base_url`) or the server's host name to it first
([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md), Which Host the listener answers). A listener with
its own certificate is unchanged.

A wrong administrator password over REST now costs a growing delay, capped at ten seconds, before the next one from
the same address is looked at - on every route that takes it, and from the loopback address too, where the
brute-force protection of the mail ports does not reach and where every browser behind a reverse proxy on the same
machine arrives. Earlier versions looked at every guess at once from there. It is a delay, cleared by the right
password: an address that keeps guessing keeps it waiting while it does, and the administrator's API keys, passkey
and single sign-on are not touched by it.

A response sent to a browser that came over HTTPS now carries `Strict-Transport-Security` for a year, which no earlier
version sent, so the browser does not go back over plain HTTP where the credentials could be read; it is never sent on
the plain loopback path, and it does not include the names under the one the browser used.

Creating or revoking a REST API key, and enrolling or disabling the administrator's second factor over REST or COM,
are now recorded on the hash-chained audit trail the Deck's Audit view reads, where earlier versions wrote them only
to the rolling application log or not at all; the key's token and the second-factor secret are never recorded.

The Windows installers, x64 and ARM64, carry OpenSSL 4.0.3, published on 29 September 2026 with fourteen advisories
against 4.0.x, where 6.3.5's x64 installer carried 4.0.2. Of the two rated above Low, CVE-2026-84782 (High) is in
DTLS, which hMailServer does not use, and CVE-2026-84783 (Moderate, in 4.0.0 to 4.0.2 only) applies to the Windows
server: a use-after-free when several threads first use the same certificate at once, which a remote peer can turn
into a crash when connections build their first chains to the same trusted CA certificate together. In the Windows
server those are the connections to a port that asks for client certificates, whose certificates are all checked
against that port's CA file. Its outgoing connections share none: the one context its SMTP, POP3 and SpamAssassin
clients share holds no trust anchor, since a remote server's certificate is checked by Windows' own chain
verification or against DANE records, and each of its other TLS clients loads its own for each connection. Of the
twelve Low ones, nine are in QUIC, DTLS 1.2, the CMP client and SM2, which the server does not use, and one in
`SSL_set_SSL_CTX` during a handshake, which it never calls. The other two are fixed with the rest: CVE-2026-35189,
the memory a certificate with many relative CRL distribution points makes OpenSSL allocate, which a server that
hMailServer connects to can send, as can a client on a port that asks for client certificates; and CVE-2026-54872, a
timing leak in ECDSA signing on curves other than NIST's. Every Windows installer from 6.0.0-B1 to 6.3.5 carried a
4.0.x that CVE-2026-84783 affects: 4.0.1 up to 6.2.24, and 4.0.2 from 6.2.25. The Linux packages and the AppImages
use the OpenSSL of the distribution they run on, and the container image its base distribution's, and take OpenSSL's
fixes from there.

The webmail now hands the browser an attachment it decrypted itself, of an S/MIME message (since 6.3.2) or an
OpenPGP one (since 6.3.4), as a file of no type (`application/octet-stream`) rather than under the type the message
named, so that the browser only ever saves it; every version from 6.3.2 to 6.3.5 is affected. Such a part that names
no file is saved as `part` with the extension of the type it declares when that type is on a short list - plain text,
CSV, a calendar, a contact card, PDF, ZIP, a key, a forwarded message, PNG, JPEG, GIF, WebP, MP3, MP4 and the three
Office documents - and as `part` alone otherwise; no extension a browser renders as a page or a desktop runs is ever
given, and the file's type stays `application/octet-stream`.

One more hardening change, found by the project's own review: a mailbox key encrypted at rest - an account's or a
domain's recovery key, opened, made or sealed again - and the keys derived from it are written over when the code
holding them is done, however it ends: a return, and now an exception thrown through it as well (from the database,
an allocation, the log), which left such a key in memory freed without being written over, in 6.3.4 and 6.3.5.

In the same way, a file the webmail saves for a second account shown beside the first is now always of no type, and
a photo it shows for that account, or an attachment its offline store answers without a connection, keeps the type
the server named only when that is an image type the page shows (or, offline, a PDF); 6.3.4 and 6.3.5 used the type
as it came.

The Control Deck is held tighter to what the server hands it, found by the project's own review. It draws an
address - the release page the update feed names, the documentation wiki's, an eDiscovery export's - as a link only
when it is an http or https address, where 6.3.4 and 6.3.5 drew the address as it came. The update check keeps a
release's page only as an https address, where every version from 6.2.28 to 6.3.5 kept whatever the feed gave. The
sign-in card shows a single sign-on error carried in the page's address only in a tab that started a sign-in at the
identity provider in the last hour and has not loaded the Deck since, inside a sentence of its own, where 6.3.4 and
6.3.5 showed the text of any such address; the webmail's sign-in page is held to the same rule, where 6.3.4 and
6.3.5 showed any such text there too. A translation that is not text, or carries a character HTML reads, is left
out when the Deck loads a language, as the build already refused it, where 6.3.4 and 6.3.5 used the server's copy
of the catalogue as it was. And the ids and the counter three of its views take from the server are drawn as
numbers whatever the answer holds.

A file sent from the webmail as a link (`/files/{token}`) is now served under the type its sender declared only
when that type is on a list of types no browser runs, applies or renders as a page - pictures other than SVG, sound,
video, PDF, plain text and CSV, the office documents and the archives - and as `application/octet-stream`
otherwise, the files links already hold included; every version from 6.3.2 to 6.3.5 refused a few named types and
served any other as declared. A contact's photo is served under its card's type only when that is one of the same
list's pictures, and as `application/octet-stream` otherwise, where 6.3.4 and 6.3.5 served it under the type the
card declared. Found by the project's own review.

The size an account declares for a file sent as a link (since 6.3.2) or for a password-protected message (since
6.3.4) is now checked against the most either may be before it is made a whole number, so that every declared size
counts against that ceiling and against the account's own; found by the project's own review.

A read of another account's mail through the REST API by the server's administrator, a domain administrator or a
role holding `view_mailbox`, `view_archive` or `view_discovery`, and a support session opened as an account, are now
recorded on the audit trail; 6.3.4 and 6.3.5, the versions with the trail, recorded none of them (Administration,
below).
When the server declines to reach an address that is not public for somebody who named it - a list's one-click
unsubscribe and a correspondent's key from their domain's Web Key Directory in the webmail, the server of another
account a reader adds there, the delivery of an inbound webhook route an API key made - the reason it gives now names
the host as it was given and never the address the server resolved it to, and for another account's server says the
same whether the name does not resolve or leads inside the network. From 6.3.2 the unsubscribe's answer, and from
6.3.4 the others, named the private address a host led to. A refusal by `HttpProxy`, which these requests go
through from 6.3.6 (What changes on upgrade, item 19), is reported to such a person as the setting's, by its status
code alone, and neither by the proxy's address nor in the words of its answer. Found by the project's own review.

Expanding a recurring event now looks each occurrence up among the event's excluded dates (`EXDATE`) by halving a
sorted list, where it went through every one of them in turn. An event can carry tens of thousands of excluded
dates in the megabyte an invitation may have, and an invitation is filed in the calendar on delivery, so such an
event cost about a third of a second each time a calendar was read over a span it covers (the webmail, CalDAV, a
room's booking check) - and, before this release, several seconds of a delivery thread for a cancellation of some
of its occurrences (What changes on upgrade, item 17), which also asks of the event a few expansions fewer than it
did. Found by the project's own review.

The Linux live update's apply step, `hmailserver-update`, which runs as root on a request the server's service account
writes, took the unit it stops from that request, and for an AppImage request the image - the program that checks the
new file's signature - as well, in 6.3.4 and 6.3.5. A service account that had been taken over could therefore have
root run a program of its choosing, on a `.deb` or `.rpm` installation with the live update's path unit started (the
packages' default) as on an AppImage. 6.3.6 takes the unit and the image from the helper's own command line alone,
refuses an AppImage request where that names no image, checks after the stop that the server's unit is down and that
nothing of the service account still runs, and verifies, runs and installs only copies it has made after the stop,
which nobody else can read until they have verified (What changes on upgrade, item 23). Windows is not affected. An
AppImage run under the path unit keeps the script copied to `/usr/lib/hmailserver/` when its update units were
installed, which the AppImage's own update does not replace: copy 6.3.6's there and give it `--image`. Until a Linux
server's helper is 6.3.6's, `systemctl disable --now hmailserver-update.path` turns the apply off, and the check and
the download go on; install 6.3.6 with the package manager. Found by the project's own review.
Some COM objects that carry no credential were not refused what only a signed-in caller may do. A message, its
attachments and a fetch account that a program creates for itself carry no credential, and neither do the objects an
`Application` handed out once a later `Authenticate` on it fails. With an administrator password set, a local Windows
user with no hMailServer credential could through them queue mail, read and write files as the service's account, and
attach a fetch account to any account. This is in every version up to 6.3.5, as in the original hMailServer. Outside
the server's own event scripts these are now refused, and reading or writing a file on the server's disk through a
message is the server administrator's alone (What changes on upgrade, item 25). Found by the project's own review.

### What changes on upgrade without a switch

1. **COM `Utilities`: the helpers that do work answer a caller that has signed in.** `BlowfishEncrypt`,
   `BlowfishDecrypt`, `ResolveMXRecords`, `GetMailServer`, `IsLocalHost`, `CheckSieveSyntax`, `EvaluateSieveScript`
   and `CriteriaMatch` refuse a COM client that has not called `Authenticate`, with "You do not have access to this
   property / method", whether it reached them through `Application.Utilities` or created `hMailServer.Utilities`
   itself. They answer, as before, an account of any level or the administrator that has signed in, and an event
   script running in the service, which needs no change. `MD5`, `SHA256`, `GenerateGUID`, `IsStrongPassword`,
   `IsValidEmailAddress`, `IsValidIPAddress` and `IsValidDomainName` stay open to any caller, and a server with no
   administrator password and no second factor answers every caller as it did. A program or script outside the
   service - a scheduled VBScript, a PowerShell script, a tool of your own - that calls one of the eight creates
   `hMailServer.Application`, calls `Authenticate` with an account or the administrator, and uses that object's
   `Utilities`: a `Utilities` created on its own has no way to sign in. The Control Panel and the shipped
   `DecryptBlowfish.vbs` already sign in. The type library changes help text only; no member or dispid is added,
   removed or moved.
2. **The live update's trust is read from `hMailServer.ini` alone.** `UpdateSigningIdentity`,
   `UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` are read
   from the file's `[Settings]` section on every platform, and never from the database, where 6.3.4 and 6.3.5 kept
   them. At the first start a value those versions stored in the database is taken out; where it said something the
   file does not, the error log names it once as `HM5809`, with both values, and from then on the file's value
   applies. A line for one of the five in `[SettingsOverride]` is ignored and named as `HM5809` at every start. COM
   and the REST API (409) refuse a change to them, and so the Control Panel, the Deck, a settings profile and the
   audit trail's put-back; a write of the value the file already holds is taken. Most servers set none of the five
   and see nothing. One that set a signer of its own over REST, the Deck or the Control Panel sets it in the file:
   on Windows with an editor run as an administrator, on Linux with `sudo hmailserver --set-setting NAME=VALUE`,
   then restarts the service. Where several servers share a database, the first 6.3.6 to start takes the rows out
   for all of them, so put the value in every server's file before the first is upgraded. `hmctl config export` and
   `Export-HmConfiguration` leave the five out, and `diff` and `apply` of an earlier export skip them with a line
   naming them; a backup does not carry them either ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "Where the
   trust is read from").
3. **The MX-record test falls back by the Public Suffix List.** When a sender's domain has no MX, the test judges it
   by its registrable domain's, which the server now finds with the Public Suffix List it already compiles in for
   DMARC (the list of 25 July 2026, its PRIVATE section included), where it cut the name to two labels, or three
   under a name in SURBL's list of 2006: `mail.example.act.edu.au` falls back to `example.act.edu.au`, not the
   suffix `act.edu.au`. A sender at a public suffix itself that has no MX - `github.io`, `co.uk` - now fails the
   test, since there is no registrant's domain above it to fall back to; under the old list `github.io` failed and
   `co.uk` passed. A name of one label is not judged, as before. Nothing to set.
4. **The SURBL test reads a link as a browser does, and a list's error answer is never a listing.** On a server
   with URI block lists (SURBL servers):
   - A link's host is the one after its user name - `http://anything@evil.example/` is asked about as
     `evil.example`, where it was `anything` - percent-decoded as UTF-8 and without its port, with a soft hyphen,
     a zero-width space and the other characters a browser deletes from a host deleted, and an IPv4 address in any
     form a browser takes is that address, an IPv4-mapped IPv6 address (`[::ffff:192.0.2.1]`) among them. A link
     with no scheme is read where a browser or a redirector follows one: an HTML attribute's value that begins
     with two slashes (`href="//evil.example/"`), and a query parameter's (`?u=//evil.example`). The host is asked
     about whole and cut to its registrable domain by the Public Suffix List, as above, and a host under a
     shared-hosting suffix also as the domain the list's ICANN section registers (`github.io` for
     `tenant.github.io`), as 6.3.5 asked. Messages whose links escaped every list in any of these ways can now be
     tagged.
   - A link to an address is asked about reversed, as a DNS list expects, and only so: it was also asked about in
     its own order.
   - The lists' error codes - `127.255.255.x`, `127.0.0.1`, `127.0.1.255` and `127.0.2.255` - never tag a message,
     whatever the expected result. A server that asked the Spamhaus DBL or ZRD with no expected result, or with
     `127.0.1.*` or `127.0.2.*`, no longer tags every message holding a link to an address. A URI list of your own
     that answers `127.0.0.1` for a listing no longer tags anything: have it answer another address, such as
     `127.0.0.2`. A list that refuses the server's queries, or answers `127.0.0.1` to every name of a message, is
     named in the application log at most every 15 minutes.
   - A host DNS cannot carry - a link that ends in a full stop, a label of 64 characters - is not asked about, where
     its failed lookup ended the list's check for the message, and the links are asked about in the order they come
     in the message, not sorted.
   - The sender lists' URL entries are matched against the same host, an address in its own order: an entry for the
     name before an `@` no longer matches, and nor does an address entry written reversed (`1.2.0.192` for a link to
     `192.0.2.1`). Write such an entry as the address itself. They are matched against every link of a message,
     where only the first 16 hosts were, so a message with an entry's link after sixteen others is now held or
     marked as the entry says.
5. **Backups leave out the live update's folder, and a destination inside the data directory works.** A backup no
   longer carries `Updates` - the installer or package waiting to be applied, the rollback image, the apply token
   and the last apply's outcome, most of the size of a backup taken before an apply - and a restore leaves this
   machine's own folder as it is and puts back no older archive's copy. A destination, the temp directory or the
   run's staging folder inside the data directory is left out of the copy, recognised by the directory itself and
   not by its spelling, and a restore leaves them, and the folder of the archive it restores from, where they are;
   the backup log names each. Up to 6.3.5 such a destination failed every backup (`HM5014`), and on Linux every
   unattended update with it; the nested copy a failed run left in the staging folder is now removed rather than
   archived. The same change removes two faults of the Linux packages, whose temp directory is inside the data
   directory, found by reading the code: an encrypted backup with messages failed the same way, and a restore of
   messages deleted the store it had just extracted. Nothing to set; a destination on another disk is still the
   better place ([Backup.md](hmailserver/docs/Backup.md), "A destination inside the data directory").
6. **`tlds.txt` is removed.** The Windows installer deletes `Bin\tlds.txt`, and the Linux packages no longer carry
   `/usr/bin/tlds.txt`, so apt and dnf remove it. Nothing reads it: a copy edited by hand has no effect any more,
   and the Public Suffix List compiled in decides. Error 4335, the list not read, is retired.
7. **The project's address.** The project is at `gitlab.com/hmailserver/hmailserver` since 29 September 2026, with
   its id, and the old addresses redirect. The Control Panel's links, the policy link of the server's
   `/.well-known/security.txt`, the systemd units' `Documentation=` and the packages' home page name it. A new
   database gets `HelpWikiBaseUrl` `https://gitlab.com/hmailserver/hmailserver/-/wikis/`; a database set up before
   keeps the value it has, as it keeps any setting - `https://gitlab.com/Progressiverobot/hmailserver/-/wikis/`
   unless an administrator changed it, which GitLab redirects, so the help panes' *Open on the wiki* still works. It
   is changed on the Control Panel's Advanced page under General, on the Deck's Settings page, as
   `help_wiki_base_url` over REST or as `Settings.HelpWikiBaseUrl` over COM ([Help.md](hmailserver/docs/Help.md),
   "The wiki link"). This release and every one after it are signed at the group's path (above).
8. **The live update keeps its outcome, tries a failed check again once, and keeps a verified installer.**
   - After an update, `Status.UpdateApplyOutcome`, `apply` in `GET /api/v1/update` and the Control Panel keep the
     outcome until the service stops or the next outcome replaces it; on 6.3.4 and 6.3.5 the check a second after
     the start erased it. The update that installs 6.3.6 is the first whose outcome stays.
   - A scheduled check that fails, when the one before it did not, is due again ten minutes later and read at the
     quarter-hourly task's next run, then every `UpdateCheckHours` as before; `GET /api/v1/update` gains
     `nextCheck`, when the scheduled check is next due.
   - An answer that ends before its headers is reported with who ended the connection, after how many bytes and
     seconds, the TLS handshake's share and how the read ended, where it said "The response carried no HTTP
     headers.", for every request of the server's HTTP client but the installer's and the bundle's downloads. A
     monitor that matches the old sentence needs the new one.
   - With `UpdateAutoDownload=1`, an installer and bundle already in the `Updates` folder are verified again where
     they are and not fetched again, and a copy that no longer passes is deleted and fetched; a service that cannot
     install an update itself (`NT SERVICE\hMailServer`, `HM7463`) no longer downloads the whole installer, 88 MB
     for 6.3.5, at every start ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
9. **On Linux the backup and event logs are UTF-8**, as the application and error logs are; they were UTF-16LE,
   which `grep` and `tail` read as every character followed by a NUL. The server converts `hmailserver_backup.log`
   and `hmailserver_events.log` in place the first time it opens each; the copies logrotate made before stay as they
   were. Windows keeps UTF-16LE.
10. **The Linux packages.** The `.deb` and the `.rpm` require `ca-certificates`, which apt and dnf install where it
    is missing. Every Linux install puts hMailServer's AGPL-3.0 text at
    `/usr/share/hmailserver/licenses/AGPL-3.0.txt` beside `QuickJS.txt`, and a copyright file at
    `/usr/share/doc/hmailserver/copyright` from the `.deb` or `/usr/share/licenses/hmailserver/copyright` from the
    `.rpm`. The `.rpm`'s `License` names every licence of what is compiled in, where it named the AGPL alone, and
    its `%changelog` is this release's entry, where it was CPack's placeholder of 2010. The update helper now runs
    `apt-get` quietly, so the journal no longer holds dpkg's progress as "[614B blob data]"; the helper already
    installed is the one that runs an update, so this shows from the update after 6.3.6.
11. **Text in the Control Panel can be selected and copied** (work item 48): on every page and dialogue by dragging,
    double-clicking, Ctrl+C or a right-click Copy, and every plain list copies its selected rows, one row per line.
    The words on the controls - a check box's or a button's caption, a drop-down's value, a tab's or a page's name,
    a label - are copied whole from their own right-click Copy or Ctrl+C; a drop-down's right-click menu, which
    offered Copy, Cut and Paste and could run none of them, now offers Copy. Live logs selects several lines,
    follows new lines only while its end is in view, and no longer splits a line the server is still writing into
    two rows. Keyboard navigation and screen-reader output are unchanged.
12. **Scripting off runs no script engine.** With scripting off the server no longer compiles the event script at
    every start, reinitialise and reload; switching scripting on compiles it first, and a script that does not
    compile is reported then rather than at each start while scripting is off.
13. **Install update under `NT SERVICE\hMailServer`, and what a script sees of it.** Where the service is not an
    administrator, the Control Panel's *Install update* installs (above), and:
    - `Status.InstallUpdate` returns false and leaves `UpdateState` at 3, with `UpdateLastError` saying who
      installs the update and where; it made the state 5, a failure, which took *Install update* away and
      offered the download again. `POST /api/v1/update/install` answers 409 with code
      `update_install_by_administrator` and the same sentence, and the state stays 3. On LocalSystem both are
      as they were.
    - `UpdateState` has two new values, 6 while the service prepares an administrator's install and 7 when it is
      ready, and COM `Status` six new members, `UpdateNeedsAdministrator`, `UpdateInstallAdvice`,
      `PrepareUpdateForAdministrator`, `UpdatePreparation`, `BeginUpdateForAdministrator` and
      `CancelUpdateForAdministrator` (dispids 20 to 25; nothing existing moves). The three that act answer only
      the server's administrator, and only from this computer. `GET /api/v1/update` adds `installedBy`
      (`service`, `administrator` or `elsewhere`), `installAdvice`, `serviceAccount`, `serverName` and
      `preparation`, and the Deck's Update page shows the sentence where its Install button stood.
    - The checklist's `update_apply_by_administrator` and `HM7463` end "installed by an administrator with
      Install update in the Control Panel on this server", where they said an administrator installs them.
      `HM7465` to `HM7469` are new, and only an administrator's install raises them; its outcome is an
      application-log line, `HM5480` when it did not succeed, and an audit event of object type `update` under
      the Windows account that approved it, interface `Windows`
      ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md)).
    - The program folder gains `AdminUpdate`, made by the first administrator's install, holding each install's
      copies, log and outcome, which only administrators can change; anyone may read the folder's own
      permissions, which the Control Panel checks before every later install's prompt. The uninstaller removes it.
      `hMailServer.exe` loads the libraries it links from `Bin` and System32 only.
14. **Smaller things a script, a monitor or a log reader may notice.**
    - `GET /api/v1/update` also answers `installKind`, `liveUpdate`, `updateCommand` and `updateAdvice`, which the
      Control Deck shows ([LiveUpdateKinds.md](hmailserver/docs/LiveUpdateKinds.md)).
    - The REST API writes to the application log which method and path it answered 500 after an exception, and the
      crash oracle's record adds, after the fields it always had, what an access violation read, wrote or executed,
      the image and offset of the fault and up to twelve frames of the faulting thread.
    - Putting a switch back from the audit trail (`POST /api/v1/audit/{id}/revert`, `Utilities.RevertAuditedChange`,
      *Put back*) sets the switch; on 6.3.4 and 6.3.5 it wrote the setting's text column, which the server does not
      read, and changed nothing.
    - Over COM, `Recipients.Item` and `DiagnosticResults.Item` refuse a negative index, a look-up that finds nothing
      no longer leaves an object behind, and a date made from a NaN reads as no date.
    - `HM4229` reports the same codes in new words, and `HM7277` names `LegacyBlowfish::EncryptToString` as its
      source.
    - A TLS scanner sees three things change (Operations, *every TLS context switched on every OpenSSL option from
      bit 32 up*): the Windows server's outgoing TLS 1.3 handshakes no longer carry an Encrypted Client Hello GREASE
      extension; TLS 1.2 handshakes in both directions offer only the uncompressed elliptic-curve point format, on
      Windows and wherever the Linux server's OpenSSL is 3.6 or later; and on Linux with OpenSSL 3.2 or later the
      MTA-STS policy fetch and the OAuth2 token request no longer offer certificate compression.
    - The error log says six failures it did not (Operations, *six failures that stopped a pass or a step*), two of
      them with new codes, `HM7554` and `HM7555`.
    - A device's count of sign-ins (`sign_ins`) stops at 2,147,483,647, the most its column holds; the next sign-in
      would have made it negative.
    - A file sent as a link whose declared type is not on the list under Security goes out as
      `application/octet-stream`, which a browser saves under the file's own name, and its record's `type` in
      `/api/v1/me/files` says so, for a link recorded by an earlier version too; one declared `text/plain` with a
      charset, which went out as `application/octet-stream`, goes out as `text/plain`. A contact's photo whose card
      declares a type that is not one of the list's pictures goes out as `application/octet-stream`, and the webmail
      no longer shows a photo in SVG.
14. **Event scripts on Linux run in QuickJS 2026-06-04**, Fabrice Bellard's release, in place of 2025-04-26. It fixes
    memory-safety flaws in the engine that a script can reach: CVE-2025-62494 (a type confusion when a string is
    added to a value whose conversion changes it, High), CVE-2025-62493 (a BigInt written as a string, Medium) and
    CVE-2025-62492 (`indexOf` and `lastIndexOf` of a typed array with a tiny negative start, Medium). It fixes two
    more, both High, that a script could not reach in the server, because each needs more memory than the 64 MiB
    ceiling allows it: CVE-2025-62495 (an integer overflow compiling a regular expression of over 2 GB) and
    CVE-2025-62496 (a BigInt made from a string of some 67 million digits or more). Each needs the script to do the
    thing; message text a script only reads and compares does not. Two advisories a scanner may list against QuickJS
    do not apply: CVE-2025-12745 (`ArrayBuffer.prototype.slice` and a buffer resized under it) is in the resizable
    ArrayBuffers that 2026-06-04 adds, and fixed there, and 2025-04-26 had none; CVE-2026-88378 (rated Critical,
    published on 24 September 2026 and fixed in no release yet) is in the reader of compiled bytecode, which the
    server never calls, and the VEX document says so. The release adds newer language features -
    iterator helpers, the Set methods, RegExp's `v` flag, modifiers and `RegExp.escape`, Float16Array, `Promise.try`
    and `Error.isError` among them - and runs scripts faster (42% on bench-v8 over the release before it, by its
    author's measure). The objects the server gives a script, the time limit and the memory ceiling are unchanged.
    Windows' VBScript and JScript are not affected.
15. **Three more web requests go through `HttpProxy`, the MTA-STS policy fetch reaches only public addresses, and on
    Windows the listeners keep their ports to themselves.** The OAuth2 token request (`OutboundOAuth2TokenUrl`), the
    MTA-STS policy fetch and the ACME client now use the server's one HTTPS client (Operations, *on Linux, some of
    the server's network waits had no timeout*), so when `HttpProxy` is set they go through it, as the update check,
    the JWKS fetch and the server's other web requests do; they went direct before. The policy fetch, whose host the
    recipient's domain names, is made only to a public address, judged by the addresses `mta-sts.<domain>` resolves
    to: a policy host at a loopback, private, link-local or otherwise reserved address is not asked, and the domain
    is treated as one with no policy, as RFC 8461 treats a policy host that cannot be reached. Through `HttpProxy`
    the proxy is asked for a tunnel to one of those addresses and not to the name, so that what it reaches is what
    was judged; a proxy that allows a tunnel only to a name refuses that, with the same outcome. The policy host is
    taken as a host name and nothing more, so a domain cannot name a port or a path for it. With it a port in
    `OutboundOAuth2TokenUrl` is used, where it was taken for part of the host name and the request failed; an MTA-STS
    policy answer longer than 64 KB is no policy, where what had been read by then was parsed; and an ACME request
    names itself `hMailServer`, not `hMailServer-ACME`, and accepts any type of answer. The token request and the
    policy fetch also take the server's TLS settings - the protocol versions, the cipher lists and the key-exchange
    groups, with TLS 1.2 still the floor - as the ACME client and the other web requests already did, and on Windows
    too each of the three is given up once its deadline has passed for the whole request (15, 20 and 30 seconds),
    where there each read had that long. On Windows the metrics, ManageSieve and ACME http-01 listeners and the two
    web listeners (the REST API with the webmail it serves, and web services) no longer set `SO_REUSEADDR`, as the
    SMTP, POP3 and IMAP listeners have not for years: given a port that another program listens on with that option,
    such a listener now fails to bind and says so in the application log, where it bound the port and reported itself
    started while the connections went to the other program. Linux keeps the option, which there only lets a listener
    start again while its last connections linger. On Windows the SMTP, POP3 and IMAP listeners, ManageSieve, the
    metrics listener, the two web listeners and the SNMP agent also claim their ports with `SO_EXCLUSIVEADDRUSE`: no
    other program can then bind one address of a port such a listener holds on every address and take the connections
    made to that address, which Windows otherwise allows a program of the same account; and a listener that binds
    every address of a port another program already holds on one address now fails to bind and says so, where it
    started and served the other addresses. A listener started again on its own address - a service restart, a
    reinitialise, a drain that is left while sessions go on - binds as before, whatever its last connections are
    doing (measured on Windows 11). It is left off in two places: a port number listed twice among the TCP/IP ports
    in one address family (on 0.0.0.0 and again on 127.0.0.1, say), both of which still bind, and the ACME http-01
    responder, which still takes every address of its port that the REST listener or another web server does not
    hold.
16. **A calendar message delivered by mail changes a stored event only from a sender the server verified.** A
    `CANCEL` is applied only from the organiser the stored event names, an updated `REQUEST` replaces the stored
    copy only from that organiser, and a `REPLY` sets only the answer of an attendee the event already lists who
    sent it - or whose delegate did, kept as `SENT-BY` in the organiser's copy. A sender at this server's own
    domains is verified by a signed-in session (SMTP `AUTH`, the webmail, a room answering by itself) as that
    account or an address it may send as; a sender from outside by a DMARC pass for exactly the domain of a `From:`
    that is the sender's address, written as that one mailbox - a `From:` that another mail program could read as
    another address, with a second `<address>`, text after the `>` or an address in a comment or in the display
    name, verifies nobody. With DMARC off, from an address range spam protection skips, through the send API -
    whatever address its key may send as - or from an external account, nothing is verified. A `REPLY` from the
    account an alias the event lists points at is set on that alias; a `REPLY` adds nobody, to the event or to an
    occurrence the organiser left the guest out of, but a member of a distribution list of this server's that the
    event lists, answering themselves, is added beside the list with `MEMBER` naming it. A new invitation is filed
    as before, marked `X-HMAILSERVER-IMIP-UNVERIFIED` when its organiser was not verified, and such a copy does not
    hold off the organiser's own invitation by its `SEQUENCE`; anything else is delivered as an ordinary message,
    with a line in the application log, and the webmail's card for it says that the calendar still has the event as
    it was instead of saying it changed ([Calendar.md](hmailserver/docs/Calendar.md), "Who may change a stored
    event"). A room or a piece of equipment believes an organiser from outside only when the DMARC pass is for a
    `From:` that is the organiser's address, and no organiser for a message sent through the send API; its own
    answers are recorded as its submission, so that an organiser's calendar on this server files them
    ([RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md), "Who is asking").
17. **A cancellation of occurrences takes those occurrences out of a guest's calendar and leaves the rest of the
    series.** A `CANCEL` whose every event carries a `RECURRENCE-ID` (RFC 5546 section 3.2.5), from the organiser
    the stored event names, takes those occurrences out of the guest's copy: each one's override is dropped and an
    `EXDATE` written on the series, and from an occurrence named with `RANGE=THISANDFUTURE` on the series ends, its
    later overrides and `RDATE`s dropped and its `RRULE` given an `UNTIL` the moment before it in place of any
    `COUNT`. The copy is marked cancelled only when those occurrences are everything it holds, as for a `CANCEL` of
    the whole event. In 6.3.4 and 6.3.5 such a `CANCEL` marked the guest's whole series cancelled. The webmail's
    card for the message says the event is no longer in the calendar once its occurrences are out, and its "Remove
    from calendar" - `POST /api/v1/me/messages/{id}/invitation/reply` with `remove`, which then answers
    `occurrences: true` - takes out those occurrences alone, where it removed the whole series
    ([Calendar.md](hmailserver/docs/Calendar.md), "Filed on delivery"). A room or a piece of equipment took
    occurrences out of a booking already.
18. **The answer from an invitation's card replaces a copy of its UID that nobody verified, whatever organiser that
    copy names.** A copy filed from an invitation whose organiser the server could not verify - marked, item 16 -
    that names another organiser than the invitation no longer makes the reader's Accept, Maybe or Decline on the
    invitation's card fail with `422`: the answer replaces it with the invitation's event, marked as the card's
    answers mark what they write. A copy planted under the UID of an invitation before it arrived, naming somebody
    else, so keeps the invitation out of the calendar only until the reader answers it. Delivery still replaces no
    copy that names another organiser, verified or not, since a sender verified as the organiser it names is not
    the owner of the UID; and a copy the server did not mark - the organiser's own, verified, one the reader or a
    phone put in, and every copy filed before 6.3.6 - still refuses an answer that names another organiser
    ([Calendar.md](hmailserver/docs/Calendar.md), "Who may change a stored event").
19. **Five more web requests go through `HttpProxy`.** A sender's brand logo, a correspondent's OpenPGP key from
    their domain's Web Key Directory, a Web Push notification, a one-click unsubscribe and the post of an inbound
    webhook route held to public addresses - one that anybody but the server's administrator made - go through
    `HttpProxy` when it is set, where until 6.3.6 they went directly whatever it said, so that a server that
    reaches the web only through its proxy could make none of them. Each is still made only to a public address,
    judged by the addresses its host's name resolves to on this server, and the proxy is asked for a tunnel to one
    of those addresses and never to the name, as for the MTA-STS policy fetch (item 15): a proxy that allows a
    tunnel only to a name refuses it, and the request fails as one with no way out. A route the server's
    administrator made, which may reach this server's own network, still posts directly.
20. **The sender lists' URL entries read a large message only as far as the anti-spam size limit.** A message
    larger than the limit past which the SURBL test and the other tests after `DATA` do not look at a message
    (`maximum_message_size_kb` of `/api/v1/settings/antispam`, COM `AntiSpam.MaximumMessageSize`, 1024 KB unless
    changed; 0 is the MIME parser's 80 MB) is judged by the links in the first part of its text and of its HTML,
    as far as that limit into each, where every character up to the parser's 80 MB was read - some 8 seconds of a
    delivery thread for a 20 MB message. A block or allow entry for a link after the limit no longer decides such
    a message, and a link the limit cuts through is left out rather than read as the part of its host before the
    cut ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
21. **An external account's *days to keep messages* means one thing for POP3 and IMAP, and the upgrade starts no
    account deleting remote mail it was not deleting.** The setting has always meant, to the POP3 fetcher, the COM API
    and the `OnExternalAccountDownload` event: -1 deletes each message on the remote server as soon as it has been
    collected, 0 leaves every message there, and a number of days N deletes a message N days after it was collected.
    From 6.2.25 to 6.3.5 the IMAP fetcher read 0 as "delete" and -1 as "keep"; the Control Panel (from 6.2.0), the
    Deck, the webmail's *Other accounts* and the REST API described that reversed meaning for both kinds of account;
    and the webmail sent -1 for *Leave the mail there*, its default. Who was affected:
    - an **IMAP** external account at 0 - the default of COM, the Control Panel and the REST API - had its remote
      INBOX emptied as it was collected, and every mirrored folder when it mirrors folders, where the setting's own
      meaning is to leave everything; one at -1, the webmail's *Leave the mail there*, kept what -1 deletes;
    - a **POP3** external account added, moved or changed in the webmail (6.3.4 and 6.3.5) with *Leave the mail
      there*, or given -1 through the REST API or the Deck, which called -1 "keeps every message", had each message
      deleted on the remote server once collected; one given *Delete it there* kept everything;
    - an event script that answered 3 ("never delete") for a message of an IMAP account had it deleted, and one that
      answered 1 ("delete now") had it kept.

    A message deleted on a remote server had been collected first and is in the local mailbox; the upgrade cannot put
    it back on the remote server. Both fetchers now ask one rule (`FetchAccount.h`), and the upgrade to schema 6106
    translates what is stored so that every account goes on doing what it did or stops deleting, and none starts: an
    IMAP account's 0 becomes -1 and its negative number 0; a POP3 account made or moved by somebody other than a
    server administrator - in the webmail, by a reader, by a key restricted to named domains or by a domain
    administrator - has a negative number made 0, so that it stops deleting what that person asked to leave; every
    other value is as it was. A backup made before 6.3.6 is restored through the same translation, and one made by
    6.3.6 as it is. What the upgrade does not change is what an event script answers: an `OnExternalAccountDownload`
    script that answers 1 (*delete now*) for a message of an IMAP account had it kept, and from 6.3.6 has it deleted,
    as the event has always said - look at such a script before upgrading. **After upgrading, look at your IMAP
    external accounts:** one that was emptying its remote INBOX against your intention now shows -1, *delete after
    download*, and goes on doing it until you set it to 0. **Look at your POP3 external accounts at -1 too:** the
    upgrade cannot tell one a server administrator set to -1 through the REST API or the Deck to keep its mail, or one
    a reader set to *Leave the mail there* in the webmail on an account a server administrator made, from one set to
    -1 through COM or the old Administrator to delete it, so it goes on deleting until you set it to 0; one a domain
    administrator made over COM with -1 to delete it, as COM has always meant, now keeps its mail until it is set to
    -1 again; and one set to 0 in the Control Panel to delete after download has kept everything, and goes on keeping
    it until you set it to -1. **A program or script that writes the value** - over COM, the REST API or the Deck -
    now has -1 delete and 0 keep for an IMAP account too; one written to the IMAP fetcher's earlier meaning is to be
    changed before it next writes it. A webmail or Deck page loaded before the upgrade says the earlier meaning until
    it is reloaded, and the webmail's sends -1 for *Leave the mail there*. The Control Panel (*Days to keep on server
    (0 = never delete, -1 = delete after download)*), the Deck, the webmail, the COM help text and the REST API's
    description now all say the same thing; the webmail shows an account's number of days that its list does not offer
    as that number, where it showed and saved back 30; and an account the webmail imports with a sign-in leaves the
    mail on the other server unless asked otherwise ([Migration.md](hmailserver/docs/Migration.md), step 3). Tests:
    `ExternalAccounts/RemoteRetention` (both fetchers at -1 and 0, and the event's 1 and 3 on IMAP and POP3, each IMAP
    case failing against 6.3.5's fetcher), `FetchAccountTester` (the rule, the 6106 translation and a backup from
    before and after it), `BackupRestore`, the 6106 step's rows on PostgreSQL and MariaDB in
    `build/db-upgrade-gate.sh`, and the webmail's and the Deck's script tests.
22. **The upgrade to schema 6107 finishes the greylisting key, and a greylisting race is never a pass** (work item
    [#50](https://gitlab.com/hmailserver/hmailserver/-/work_items/50), the follow-up to GitHub discussion 281). Schema
    6072 left the triplets written before it without a key, to be found by their first 200 characters until they were
    matched once or expired - a second query for every new triplet for as long as one remained. The upgrade now gives
    them their key in the server, since no database can compute it, with the function new rows are written with;
    deletes the rows in which an address had been cut to the old columns' 200 characters, since such a row may stand
    for several senders and its key would match nobody; and leaves a triplet one row, the one that passed most. Then
    both key columns are `NOT NULL` and the key is unique - on MySQL and MariaDB over a stored generated column,
    `glipaddress2key`, and on PostgreSQL as two partial indexes, because both count one NULL `glipaddress2`, an IPv4
    triplet's, as different from another - and the lookup asks for the key and nothing else. A sender whose row was
    deleted is greylisted once more; the upgrade's output says how many rows were keyed and deleted, and why (with
    DBUpdater and `UpgradeOnStart`, in the application log when application logging is on). On a large
    `hm_greylisting_triplets` the keying runs before the step's transaction, each statement committed as it goes, so
    an upgrade stopped part-way keeps what it did and the next run continues from there. And when two connections
    present the same new triplet at once, the unique key refuses the second insert, which is read back and answered as
    the triplet that is there: deferred and counted while it is pending, let through once it has passed - never let
    through for the refusal ([Greylisting.md](hmailserver/docs/Greylisting.md), "The triplet key").
23. **The Linux live update's helper takes the unit and the AppImage from its own command line.**
    `hmailserver-update` stops `hmailserver.service`, or the unit its `--unit` option names, and refuses a request
    whose `unit=` line names another. It takes an AppImage request only when its `--image` option names the image,
    which is then the program that verifies the new one, and refuses a request whose `image=` line names another
    file, and a `.deb` or `.rpm` request where `--image` is given. After the stop it checks that the unit is
    inactive and that no process runs as the owner of the Updates directory or of a directory above it, and
    otherwise starts the unit again and does nothing else. It verifies, reads and installs copies of the package and
    the rollback image, made once the service has stopped and only of plain files still inside the Updates
    directory, under `/var/tmp` (an AppImage's beside the image), which needs room there for both. A machine whose
    package database, or image, does not say which version is installed is no longer updated "over whatever is
    here": the outcome is `failed`. A server installed from the `.deb` or the `.rpm` under the packaged unit needs
    nothing. One under a unit of another name, through the path unit, needs `--unit` in a drop-in for
    `hmailserver-update.service`; an AppImage under the path unit needs `--image` there, and 6.3.6's script in
    `/usr/lib/hmailserver/`, which the AppImage's swap does not replace; an AppImage that runs as root under a unit
    of its own passes both itself. The update to 6.3.6 is applied by the helper of the version it replaces, and
    6.3.6's own applies the next one
    ([packaging/README.md](hmailserver/source/Server/platform/packaging/README.md), An AppImage).
24. **Port 25 still takes AUTH, and the setup checklist says so** (GitLab work item #54). The schema step 6107 to
    6108 gives every TCP/IP port an SMTP authentication and writes On into every row, so no listener of an upgraded
    server answers `AUTH` differently, and nobody is banned on the morning of the upgrade: clients that still sign in
    on 25 go on doing so. The setup checklist has a thirteenth item, *No password on port 25*
    (`delivery_port_auth`), action needed with no one-click fix while an SMTP listener on 25 is On - setting it to
    Honeypot or Off is what arms the ban, so move such clients to 587 first
    ([SmtpAuthOnDeliveryPorts.md](hmailserver/docs/SmtpAuthOnDeliveryPorts.md)). `GET /api/v1/ports` gives each port
    `smtp_authentication`, and a `PUT` of a port that leaves it out writes `on`, as it writes the default of every field
    it leaves out. `TCPIPPorts.SetDefault` - *Restore defaults* - writes both SMTP ports On, and says so. A backup
    taken before 6.3.6 restores every port On. The type library adds `TCPIPPort.SMTPAuthentication` (dispid 12). The
    error HM6099, a ban's range that could not be saved, no longer says the address reached the failed-logon limit,
    which a spam trap's or a null sender's ban never did.
25. **COM: a message or fetch account a program creates itself, and the objects of an `Application` whose later
    `Authenticate` failed, are refused outside an event script.** With an administrator password set, `Message.Save`
    and `Copy`, `FetchAccount.AccountID` and `Save`, `Account.Save` and the other writes to an account's rules,
    folders and folder permissions refuse an object that carries no credential - a `hMailServer.Message` or
    `hMailServer.FetchAccount` a program created with `CreateObject` or `new`, or an object an `Application` handed
    out before a later `Authenticate` on it failed. `Attachments.Add`, which reads a file from the server's disk, and
    `Attachment.SaveAs`, which writes one, answer the server administrator alone: a domain administrator or an
    account that signed in is refused them. The server's event scripts, which create these objects without signing
    in, work as before, and a server with no administrator password and no second factor answers every caller as it
    did. A program outside the service that sends mail through COM - a scheduled VBScript, a PowerShell script, a
    tool of your own - creates `hMailServer.Application`, calls `Authenticate`, and makes its message with the new
    `Application.CreateMessage()` where it called `CreateObject("hMailServer.Message")`; to attach a file it signs in
    as the administrator. A .NET program built against an earlier type library reaches `CreateMessage` once rebuilt
    against 6.3.6's, or late-bound ([Roles.md](hmailserver/docs/Roles.md)).

### Downloads and the release

- **The AArch64 `.deb` and `.rpm` are in the release again**. The releases up to 6.3.3
  carried them; 6.3.4 and 6.3.5 did not, so an arm64 Debian, Ubuntu or RPM server has had no newer file than
  6.3.3. The release now collects `hmailserver_<v>_arm64.deb`, cross-compiled by `deb-ubuntu26.04-arm64` in
  Ubuntu 26.04's own container, and `hmailserver-<v>-1.aarch64.rpm`, `rpm-fedora44-aarch64`'s copy of its Fedora
  44 build - the names an installed arm64 server's update check asks for - each with its own CycloneDX SBOM
  (`<file>.cdx.json`), a line in `hmailserver-linux-<v>-SHA256SUMS.txt` and a Sigstore bundle, checked by
  `release-verify` with the server's own verifier like every other file.
  `release` waits for each package's build, its SBOM job and its install job, which installs and runs it in an
  AArch64 container of its distribution under qemu-user; no AArch64 machine has run them. A package job's
  container has neither PowerShell nor Python, so every package build now records what it was built against
  (`built-against/`: what dpkg, or the aarch64 sysroot's rpm database, held, and Boost's header), and the new
  `deb-ubuntu26.04-arm64-sbom` and `rpm-fedora44-aarch64-sbom` jobs make the SBOM from that in the CI image
  (`build/ci/packages/package-sbom.sh`; `build/asset-sbom.py make --dpkg-list` or `--rpm-list`). In a cross
  build the C and C++ libraries are named as the cross toolchain's, and every other library as the target
  architecture's own `-dev` package, never the build container's. `collect` holds each AArch64 package to the
  sums its build job wrote, and `release-publish` checks that the feed gives Debian and RPM servers on AArch64
  their file and bundle, as it does for x86-64. The x86-64 files, their SBOMs and `linux-build`'s two lines of
  the sums are unchanged; `linux-repro` still compares the x86-64 packages alone, and the AArch64 ones are
  built once and not compared (ReproducibleBuilds.md). `build/check-release-chain.py` now holds `release` to
  those six package jobs, none of which may be allowed to fail or wait for a press in a tag's pipeline, and
  `collect` to where they leave the files; the update feed's `/download/hmailserver_arm64.deb` and
  `/download/hmailserver.aarch64.rpm` follow the first stable release that carries them, and its
  `--serve-check` fails a stable release that carries any server's file without its bundle. The generic arm64
  `.deb` goes to the apt suite `ubuntu26.04`, as the amd64 one does; the generic `.rpm`s still go to no dnf
  repository, and the pages job no longer calls the aarch64 one built on Ubuntu (ContinuousIntegration.md,
  "What a release carries"; PackagesDeb.md; PackagesRpm.md).

- **The container image is pushed from the release's own pipeline, by hand, and started before it is left there**
  (6.3.6's own pipeline is the first to run these jobs, which no branch's pipeline has). The image jobs are in
  every protected `v*` tag's pipeline. `container-image-deb`,
  started by hand after `release-publish` once the project variable `HM_PUBLISH_IMAGE` is 1, takes the `.deb` and its
  SBOM from the published release and verifies both with cosign as the tag's pipeline. The variable is read as the
  jobs run, not by a rule, so a release tagged before it is set has its image pushed from its own pipeline
  afterwards; 6.3.5's own pipeline has no image jobs, so the first image is 6.3.6's. The image is pushed only at
  `registry.gitlab.com/hmailserver/hmailserver`, which the Compose file and the chart now name (`linux-image.sh`
  and `container-image-push.sh` refuse the old path), then signed, scanned and started beside PostgreSQL by
  `container-image-start`, which checks SMTP and the REST API. The image sets `HM_INSTALL_KIND=container` and
  carries OCI and Artifact Hub labels ([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

- **The container image for linux/arm64 as well as linux/amd64, one index over both**. Each platform's image is built
  from the release's `.deb` of its own architecture: the Dockerfile takes the `.deb` of `TARGETARCH` and runs
  `hmailserver --version` at the end of its build, so an image whose server cannot run on its platform is never made.
  Each platform has its own kaniko job on linix, the arm64 one under qemu-user: `linux-image-arm64` beside
  `linux-image` in a branch's pipeline, and `container-image-amd64` and `container-image-arm64` in a tag's.
  `container-image` joins the two under one Docker manifest list with regctl, the tool the channels already take from
  Docker Hub, and pushes nothing until both images and the index have passed their checks; no tag names anything but
  the index. `:latest` and `:<major>.<minor>` only move forward: a release whose image jobs run after a newer
  release's still pushes `:<version>`, and leaves either tag where it is when the image it names says it is a newer
  version, or its version cannot be read, which the job's log says. The index and each platform's image are signed,
  each image has its own SBOM attested to it, and each is scanned and started beside PostgreSQL, the arm64 one under
  emulation. By hand on linix, an arm64 image of 6.3.5, from batch41's arm64 `.deb` (the 6.3.5 release carries none),
  built in about three minutes and passed the start test ([ContainerImage.md](hmailserver/docs/ContainerImage.md)).

- **The Helm chart as a file of each release, served from `https://updates.progressiverobot.com/helm/`**.
  The release carries it, signed, with its `.sigstore.json` copy. `helm-chart-package` packages `hmailserver-chart-<version>.tgz` at the release's version,
  naming the release's image, and `build/ci/helm-repo.py` writes the repository's index and Artifact Hub's metadata
  from the update feed in `package-repos`.

- **`hmctl` as a PyPI package, `hmailserver-cli`, and the PowerShell module with a Gallery manifest** (neither
  published yet: the two accounts do not exist). `build/ci/cli-packages.py` builds `hmailserver-cli`'s sdist and wheel from
  `hmailserver/source/Tools/Cli/python/` with the same `hmctl` file the `.deb` and the `.rpm` install, copied into it
  byte for byte, at `Version.h`'s version, with no dependency and no code that runs when it is installed; it runs
  on Python 3.8 and later. `HmailServer.psd1`, the module's new manifest, lists its 95 functions for Windows
  PowerShell 5.1 and PowerShell 7 at the server's version, which the release's version stamp now includes. The
  manual `master` jobs `pypi-publish` (a PyPI trusted publisher, with PyPI attestations) and `psgallery-publish`
  (`PSGALLERY_API_KEY`) say "not configured" until each account is set up. They publish only a release the update
  feed lists, rebuilt from its owner-signed tag, and never a version twice: `pypi-publish` finishes an upload that
  stopped halfway and fails when a PyPI publisher that once worked stops matching, and `psgallery-publish` refuses
  a module with no Authenticode signature, which the release does not sign yet
  ([CommandLinePackages.md](hmailserver/docs/CommandLinePackages.md)).

- **A source tarball with nothing non-free in it, which the Linux build and suite run from** (the release
  carries it from 6.3.6, below). `build/ci/source-tarball.sh` writes `hmailserver-<version>.tar.gz`
  from a commit with `git archive`: the commit's tree less every path of `build/source-exclusions.txt` - SQL Server
  Compact 3.5's headers and both copies of its licence, which are not free, and what only the Windows build and its
  installer use (the rest of `libraries/` with the Fat Cow icons, the COM API, the Windows service host and
  helpers, the vendored zlib, the Control Panel and the database tools, the VBScript add-ons, the .NET Framework
  developer tools and the installer's sources, keeping the Control Deck and `dh2048.pem`, which the packages
  install). It is reproducible - every entry carries the commit's time, uid and gid 0 and a 0644 or 0755
  mode, line endings are `.gitattributes`' alone (no system attributes file is read, and a clone whose
  `.git/info/attributes` sets any is refused), gzip writes no name or time - and git 2.39.5, 2.47.3, 2.50.1 and
  2.53.0 with gzip 1.10 to 1.14 made the same bytes. QuickJS stays and is declared with the other third-party
  notices of the code the Linux build compiles; `docs/SourceTarball.md` also lists the third-party test data kept
  (Mail::DKIM's corpus, and captured messages with no licence stated, for the Debian package to decide on).
  `build/check-source-tarball.py` holds the list to the tree in the `checkers` job (nothing the CMake build,
  `linux-build.sh`, `linux-suite.sh`, the other Linux jobs' scripts - packages, fuzzing, cross, sanitizer,
  coverage, repro - the suite's project and its directory, what the build and the suite read at run time, or a
  POSIX `#include` reads may be left out, and every file of the Windows-only trees must be) and a tarball to the
  list, its licences and its format; `--files-excluded` prints Debian's `Files-Excluded` field from the same list.
  The `source-tarball` job makes it twice and compares, and `linux-build` and `linux-suite` take it with
  `HM_SOURCE_TARBALL`; `build/native-dependencies.ps1 -LinuxOnly` gives the packages' SBOMs their components
  without the Windows build's inputs. See [SourceTarball.md](hmailserver/docs/SourceTarball.md).

- **The release carries every file the pipeline makes for it**. From 6.3.6 a release is 60 files and 154 links: the
  x64 and ARM64 installers (the ARM64 one below); the generic `.deb` and `.rpm` for x86-64 and AArch64; each Debian 12
  and 13, Ubuntu 24.04, Fedora 44 and 43, EL 10 and 9, openSUSE Leap 16.0 and Tumbleweed release's own package for both
  architectures (`hmailserver_<v>-1~<release>_<arch>.deb`, `hmailserver-<v>-1.<release>.<arch>.rpm`); both AppImages,
  with the project's own runtime (work item 36), the source of the libraries linked into it and the source of the
  libraries they carry in `usr/lib` (below); the source tarball; the Helm chart; the sums of every Linux file, the
  two AppImage sources files and the chart; an SBOM for each of the 26 compiled files and the tarball, and the tree's
  two; a Sigstore bundle beside each, a byte-identical `<file>.sigstore.json` of each primary file's bundle, and
  `hmailserver-<v>.intoto.sigstore.json`, SLSA v1 build provenance naming every file by its SHA-256, signed as the
  pipeline with `cosign attest-blob` and checked with `cosign verify-blob-attestation` (SECURITY.md, "Build
  provenance"). The new `release-packages` job gathers the per-distribution packages, the AppImages, the two AppImage
  sources files and the tarball, holds each to its build's sums and makes the SBOM of each but the sources files
  (`build/asset-sbom.py` gains the kinds `appimage`, which lists what the image bundles and what its runtime links,
  and `source`), so that `release` stays under GitLab's fifty needs. Before anything is signed, and again before
  anything is uploaded, the release counts what GitLab and Pages take: every link's path (the `~` of a `.deb`'s
  version, which GitLab refuses there, is written `-`), `_redirects` under 1,000 rules and 64 KiB with the apt and
  dnf rules, and the feed under the 4 MiB a server reads (with the repositories as `package-repos`' `--keep` keeps
  them). The apt and dnf suites will serve the new packages once the repository key exists; openSUSE's go through
  OBS. A protected `batch*` branch rehearses the chain (`.gitlab/ci/release-rehearsal.yml`): collected with
  `windows-packages-x64`'s installer, signed as the branch, which no server trusts, and checked with the server's own
  verifier, which must refuse every bundle at its identity; nothing is published (ContinuousIntegration.md, "What a
  release carries", "The release rehearsal").

- **The source of the AppImages' runtime's libraries goes out with them**. Each
  AppImage's runtime is linked statically on Alpine, and two of the libraries it takes in, libfuse3 (LGPL-2.1) and
  lzo (GPL-2.0-or-later), ask for their own source to go with the binary, not only the runtime's; with lzo's GPL, so
  does every other library linked in but the compiler's own. From 6.3.6 the release carries
  `hmailserver-<v>-appimage-runtime-sources.tar.gz`: for libfuse3, lzo, squashfuse, zstd, lz4, xz, zlib and musl, at
  the version and aports commit each runtime job recorded from the linker's map and apk's database, Alpine's APKBUILD
  and patches from gitlab.alpinelinux.org, fetched by the commit's id, and the upstream archive from Alpine's
  distfiles mirror - never the GitHub releases several APKBUILDs name - each source held to the SHA-512 its APKBUILD
  gives, with a README.txt saying where each file came from, what each licence asks and how to link the runtime again
  with a changed library. The new `appimage-runtime-sources` job packs it reproducibly, twice, and compares; the
  release gathers it with a line in the sums, a bundle and a `.sigstore.json` copy, and no SBOM, being source. Each
  image's `usr/share/hmailserver/licenses/appimage-runtime.txt` lists what its runtime links and names the file, and
  the image carries each library's licence files in `usr/share/hmailserver/licenses/appimage-runtime/`, with the
  GPL-3.0's text, under which the runtime takes lzo (runtime.c being AGPL-3.0-or-later). GCC's start-up files and
  libgcc stay out, under the GCC Runtime Library Exception
  ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md), "Licences"; work item 36).

- **The AppImages carry the licences of the libraries in their `usr/lib`, and the release their source**.
  Each AppImage carries in `usr/lib` the shared libraries the server needs that are not left to the
  host - libpq, libldap and what they need, nineteen files from fourteen Ubuntu 24.04 source packages - and seven of
  them are under the LGPL (GnuTLS, Nettle, GMP, libidn2, libunistring, libtasn1 and keyutils), which asks for the
  library's source to be offered with the binary. From 6.3.6 each image carries, in
  `usr/share/hmailserver/licenses/`, the copyright file of every package a library came from, every licence text
  those name (a list in braces included), `bundled.txt`, which lists each library with its package, version and
  source package and names the release file with their source, and hMailServer's own AGPL-3.0 text (`AGPL-3.0.txt`).
  The release carries `hmailserver-<v>-appimage-bundled-sources.tar.gz`: all fourteen source packages (`.dsc`,
  upstream archives, Ubuntu's changes) at the versions the images took, from Ubuntu's archive or, for a version the
  archive has superseded since - as a package the pinned build image already had can be, which the build does not
  upgrade - from its snapshot service at the time the image's dpkg installed it, each file held to the `Sources`
  index of an `InRelease` verified against Ubuntu's archive keys and again to its `.dsc`, with a README.txt saying
  what each licence asks, where each package was taken from and how to run an AppImage with a changed library; about
  75 MB, one file for both architectures. The new `appimage-bundled-sources` job packs it reproducibly, twice, and
  compares; the release gathers it like the runtime's sources, with no SBOM. Each AppImage's SBOM now lists every
  Alpine package its runtime links statically, with its aports commit and licence, and each bundled library's source
  package; a library an image starts to carry fails the image's own job until its licence is named in
  `build/ci/packages/appimage-bundled-sources.sh` ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md),
  "Licences"; work item 36).

- **The release channels, each published by hand from `master`, dormant until its account exists** (none live as
  6.3.6 is released).
  `.gitlab/ci/channels.yml` adds `registry-mirror` (the image by digest, with its cosign signature and SBOM
  attestation, to Docker Hub, Quay.io and optionally Amazon ECR Public, with `regctl`), `aur-publish` (the AUR's
  `hmailserver`), `ppa-upload` (the Debian source package to `ppa:hmailserver/hmailserver` for Ubuntu 24.04 and
  26.04), `copr-build` (Fedora Copr through its webhook; `.copr/Makefile` makes the source RPM in Copr's builder),
  `obs-publish` (the openSUSE Build Service, from `platform/packaging/obs/_service`), `chocolatey-release` with
  `chocolatey-publish` (packed on bench150), `snap-upload` (edge and candidate) and `channels-verify`, the dry run.
  Each publishes the release `HM_CHANNEL_VERSION` names or the update feed's latest (Copr and OBS the latest alone),
  verifies every release file it takes against its Sigstore bundle as the tag's pipeline - the project's id, read
  from the certificate's extensions, required at the group's path - before it logs in or sends anything, and never
  moves a channel back to an older release. The snap, not a release file, is held to the verified tarball or the tag;
  OBS fetches the tarball itself once the job has checked the permanent link serves the verified bytes. cosign and
  regctl come out of their images pinned by digest; nothing from GitHub, and no credential on a command line
  ([Channels.md](hmailserver/docs/Channels.md)).

- **The Arch package builds from the release's source tarball**, not a git tag, as a distribution's build
  (`-DHM_LIVE_UPDATE=OFF`, naming `makepkg -si` from the AUR as its update) and without LTO (Arch's default full-LTO
  link of the server passed 6 GB and was killed); `aur-publish` writes the verified tarball's SHA-256 in.

- **The Chocolatey package's id comes from `make-package-manifests.ps1 -ChocolateyId`**: `hmailserver`, or
  `hmailserver-progressiverobot` if upstream's maintainers do not add the project. Its addresses are the group's.

- **The release checks read the signing certificate's fields from its extensions, never by searching its bytes, and
  only from the certificate cosign verified.** release-verify.sh and container-image-deb.sh took the project id
  (Fulcio's 1.3.6.1.4.1.57264.1.15) from wherever its bytes first appeared, and a certificate's public key is bytes
  its holder chooses; registry-mirror read the id from whichever signature certificate held the identity's bytes, and
  check-provenance matched the identity the same way. One reader, build/ci/sigstore-certificate.py, now walks the
  certificate's DER to its extensions, with a shell twin for cosign's image held to it case by case. The shell reader
  reads a v0.3 bundle only - cosign verifies just the first certificate of a v0.1 or v0.2 chain, and accepts field
  names spelled in ways a count of `rawBytes` misses - container-image-deb.sh refuses a cosign without
  `--new-bundle-format`, and registry-mirror refuses a signature manifest Go's JSON reader would read otherwise (a
  key in another case or with a long s, a key twice, an index at the `.sig` tag); the Python reader refuses a
  duplicated key. build/check-release-chain.py holds every script to the readers and refuses the Sigstore OID written
  as bytes, decimal numbers or base64 anywhere else. The container image's mirror has cosign verify its signature and
  SBOM attestation from one copy of their manifests, each fetched by its digest, reads the project's id from those
  same manifests - the attestation's certificate now held to it as the signature's is - and puts exactly those
  manifests at each registry, replacing whatever signature was there; the release's SLSA provenance is held to the
  project's id too, as its files are; and neither certificate reader relies any longer on cosign refusing a malformed
  bundle to know which certificate it read. The server's own verifier reads the extension through OpenSSL and was
  never affected.
- **An Atom feed of releases** at https://updates.progressiverobot.com/releases.atom: every release, newest first,
  the first paragraph of its notes as the summary, ids that do not change when the project moves into the group;
  build/check-atom-feed.py holds it to RFC 4287 and to releases.json in the pages job
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "The Atom feed"). The files a server reads are unchanged, byte
  for byte, and a release whose name XML cannot carry is entered under its tag rather than stopping the feed.
- **Releases pause after 6.3.6; security fixes do not.** The 6.3 line receives security fixes until 30 September
  2027, each in a release of its own ([SECURITY.md](SECURITY.md), "While releases pause"; [RELEASE.md](RELEASE.md),
  "A security release during the pause").
- **The container image's mirror checks each registry as anyone would see it.** `registry-mirror` verifies the image
  again at each destination, which Channels.md calls anonymous; it read with the regctl configuration it had logged
  in with and ignored whether the logout worked, so a failed logout left the check logged in, where an image or a
  signature only the account can see would pass. The check now reads with a configuration of its own that no login is
  written to, and a failed logout is said in the log; Channels.md says a registry's earlier signature or attestation
  for the image is replaced by the verified one.
- **A Debian source package and an RPM spec, built from the release's source tarball as a distribution builds them.**
  `hmailserver/source/Server/platform/packaging/debian-src/debian/` builds `hmailserver` and `hmailserver-cli`
  (debhelper 13) from `hmailserver_<version>+ds.orig.tar.gz`: the release tarball repacked by `Files-Excluded`, which
  is the project's exclusion list plus five test files that state no licence and the Debian build does not read.
  `platform/packaging/rpm/hmailserver.spec` builds the same two packages for Fedora, EL and openSUSE from the tarball
  as it is. Both are built without the live update, naming `apt upgrade hmailserver`, `dnf upgrade hmailserver` or
  `zypper update hmailserver` instead, and link the distribution's shared Boost (`-DHM_BOOST_STATIC=OFF`, new; the
  project's own packages keep it static). The account comes from `sysusers.d`, the unit is installed and not started,
  `hmctl` is a package of its own that depends on `python3`, and the server's `--version` and the tester programs run
  in the build, and neither links with LTO. The Debian package has a DEP-5 copyright over every third-party notice; a
  watch file on GitLab's releases API; and an autopkgtest that starts the server against a throwaway PostgreSQL and
  asks SMTP and IMAP for their greetings. The spec declares `Provides: bundled(quickjs) = 2025.04.26`. New CI jobs
  (`.gitlab/ci/source-packages.yml`, `batch*` and `master` only) build both for Debian 13, Ubuntu 24.04 and 26.04,
  Fedora 44, EL 10, Leap 16 and Tumbleweed with the network refused, fail on a lintian or rpmlint error, and install
  and start the result in a clean container. On the laptop the spec built on Fedora 44 and EL 10 and each server
  installed and started against PostgreSQL, and the Debian package built on Ubuntu 26.04 and passed its autopkgtest;
  a Fedora 44 server that died before `main` there was a file the laptop's WSL file system had corrupted, not a GCC
  16 or code defect. See [SourcePackages.md](hmailserver/docs/SourcePackages.md).
- **A Nix flake of the project's own: `nix run gitlab:hmailserver/hmailserver`**.
  `flake.nix` at the top of the repository builds the Linux server as a Nix package, `packages.<system>.hmailserver`
  for x86_64-linux and aarch64-linux, with nixpkgs' Boost, OpenSSL, libpq, OpenLDAP and zlib, the live update off
  naming `nix flake update hmailserver`, the MariaDB connector on the server's RUNPATH for the `dlopen` a MySQL
  database needs, and the server's `--version` and every tester program in `checkPhase`; `hmctl` is its `cli` output.
  nixpkgs is NixOS 26.05's release tarball from releases.nixos.org, pinned by its NAR hash in `flake.lock` and
  imported as a plain tree, never `github:NixOS/nixpkgs`, and the flake registry is not read. `nixosModules.default`
  is `services.hmailserver`: the server under systemd with the packaged unit's sandbox;
  `/etc/hmailserver/hMailServer.ini` with its directories and database set from the options at each start and every
  other line kept, the stored-secrets key among them, and the database password too when no password file gives
  one; a local PostgreSQL reached over its socket by peer authentication, root mapped to the same role so that the
  packaging guide's `sudo hmailserver` commands work, and the server started after `postgresql.target`, once
  PostgreSQL's setup has made that role; the database made and upgraded before each start and the first start's
  secure defaults; and options for another database, an administrator password file, settings, the firewall, and
  the auto-ban's reaching the firewall (`autoBanFirewall`, below). `checks.<system>.nixos` starts that machine and
  asks SMTP (with STARTTLS) and IMAP for their greetings, as the Debian package's autopkgtest does, and the REST API
  for its status, has root make the stored-secrets key, holds the service to no restart before that, and asks again
  after a restart, which must keep the key and a database password typed into the file; then it places an auto-ban
  through the REST API and requires the address in the hook's nftables set, and out of it once the ban is deleted.
  The new `nix` job (`.gitlab/ci/nix.yml`, `build/ci/nix.sh`; master and `batch*`) holds the lock to
  releases.nixos.org, evaluates every output for both systems, builds the package and runs the NixOS test under
  QEMU's TCG, since linix has no KVM. On 29 September 2026 the test passed with KVM and under TCG, and the job's own
  script passed end to end on linix, without KVM, in 27 minutes. See [Nix.md](hmailserver/docs/Nix.md).
- **Alpine Linux: the server on musl, an OpenRC service and an APKBUILD**. The server had
  never been built on musl; on Alpine 3.24 four things were musl's, each fixed where it was. `File.cpp` took
  `strerror_r`'s answer as glibc's GNU one gives it, and musl's is the POSIX one. The server named US-ASCII to iconv
  as `ANSI_X3.4-1968`, glibc's name, which musl's iconv does not know, so a `us-ascii` part did not convert and a
  script's text added to an empty body was lost; it is now `US-ASCII`, which every iconv knows. And musl's iconv has
  no UTF-7, in which IMAP folder names are written: `CMakeLists.txt` now asks the iconv it found for UTF-7 and
  US-ASCII and refuses one without them, naming GNU libiconv, which the Alpine package builds with. And the tree
  writes a `size_t` as MSVC's `%Iu`, which the POSIX build handed to the C library as it was: glibc read the `I` as a
  flag of its own and printed the size, cut to 32 bits, and musl refused the format and wrote nothing, so every IMAP
  FETCH item sent as a literal (`BODY[...] {n}`, `BINARY`, `PREVIEW`) lost its name and size; `%I` is now rewritten
  as C's `%z`, which also mends a size above 4 GB on glibc. `platform/packaging/alpine/APKBUILD` (with
  `hmailserver.pre-install`, which makes the account, and `hmailserver.post-upgrade`, which upgrades the schema)
  builds `hmailserver`, `hmailserver-openrc` and `hmailserver-cli` from the release's source tarball with Alpine's
  libraries, the live update off naming `apk upgrade hmailserver`, depends on `ca-certificates-bundle` as the other
  packages depend on `ca-certificates`, and runs the server's `--version` and every tester in `check()`;
  `apkbuild-lint` has nothing to say about it, and `build/check-linux-version-stamp.sh` holds its `pkgver` to
  `Version.h`, so stamping a release now edits it too (RELEASE.md).
  `platform/packaging/openrc/hmailserver.initd` runs the server as `hmailserver` with `CAP_NET_BIND_SERVICE` ambient,
  no new privileges and umask 0027, applies the first start's secure defaults, reloads on SIGHUP and gives a stop
  five minutes to drain; `hmailserver --create-database` says, where OpenRC and that service are installed, that the
  service's next start secures the new database, where it said only the systemd unit's first start would. New CI
  jobs (`.gitlab/ci/alpine.yml`, `build/ci/alpine.sh`; master and `batch*`):
  `alpine-package` runs the APKBUILD with abuild on the source tarball, `alpine-install` installs the packages in a
  clean container, starts the server with `rc-service` against PostgreSQL and asks SMTP (with STARTTLS) and IMAP, and
  `alpine-suite` runs the whole Linux suite against the installed server in Alpine, with Alpine's .NET SDK. On linix
  on 29 September 2026 the package built and passed `check()`, the install and start passed, and the whole Linux
  suite passed against the installed server with the counts batch42's glibc suite had that day: 1897 passed, 331
  skipped, none failed, of 2228. The aports merge request is a named maintainer's (Roadmap3, "Alpine Linux"). See
  [Alpine.md](hmailserver/docs/Alpine.md).
- **The `.deb` and the `.rpm` carry hMailServer's licence and a copyright file**. Until
  now the release's `.deb` and `.rpm`, and every per-release package built with them, carried QuickJS's licence and
  nothing of the project's own: no AGPL-3.0 text, which the AGPL's sections 4 and 6 ask to go with every copy, and no
  copyright file, which Debian Policy 12.5 asks of every package; only the Arch package installed `LICENSE`. From
  6.3.6 every Linux install puts hMailServer's AGPL-3.0 text, the FSF's file byte for byte, at
  `/usr/share/hmailserver/licenses/AGPL-3.0.txt` beside `QuickJS.txt`, as the AppImages carry it; the `.deb` carries
  `/usr/share/doc/hmailserver/copyright` and the `.rpm` `/usr/share/licenses/hmailserver/copyright` - the Debian
  source package's machine-readable copyright file, with the AGPL in full and the terms of what is compiled into the
  server - and the `.rpm` marks all three `%license`, so that `rpm -qL` lists them. Its `License` now names the
  licences an identifier can name,
  `AGPL-3.0-or-later AND MIT AND BSD-1-Clause AND MPL-2.0 AND LicenseRef-Fedora-UltraPermissive AND BSL-1.0`, where
  it named the AGPL alone. `build/ci/packages/check-licenses.sh` holds the three files to the tree by SHA-256, mode
  and owner: `linux-build`, `deb-build.sh` and `rpm-build.sh` refuse a package without them, and every install test
  checks them as installed ([PackagesDeb.md](hmailserver/docs/PackagesDeb.md),
  [PackagesRpm.md](hmailserver/docs/PackagesRpm.md), "The licences").
- **The backup index's XML is the project's own code; XMLite is gone.** The code that writes and reads
  `hMailServerBackup.xml`, writes a person's `account.xml` and reads the class tests' `test_config.xml` was XMLite
  (Cho Kyung Min, The Code Project, 2002-2004), which carried only its author's notice - one a distribution's legal
  review would have had to settle, and which the project could not relicense. `Common/Util/XmlTree.cpp` replaces it,
  written anew to a description of what XMLite wrote and read and keeping the callers' names; the description was
  taken from XMLite's code by the same hand that wrote the replacement, so it is not a clean-room reimplementation,
  and none of XMLite's code is in it. It writes an index byte for byte as XMLite did, and reads and refuses exactly
  what 6.3.5's XMLite did, so every archive 6.3.5 restores still restores; four indexes written by 6.2.13 and 6.3.3,
  read and written back through the server's own file code, come back byte for byte. A differential harness compared
  old and new over about 17 million cases each with GCC 15, clang 21, and clang with AddressSanitizer and
  UndefinedBehaviorSanitizer, and 7 million with MSVC, and found no difference. `XmlTreeTester` runs in ClassTester
  and, on Linux, as the new `xml-tree-tests` program, which the Debian and RPM source-package builds also run. HM4229
  reports the same codes in new words. `debian/copyright`, the RPM spec and the source-package documents no longer
  declare XMLite, and `check-source-tarball.py` now refuses a `debian/copyright` paragraph whose files are gone.
- **The date class is the project's own.** `HM::DateTime`, which every stored date, expiry, IMAP UIDVALIDITY and MIME
  date passes through, was a class of 1999 from CodeGuru that stated no licence and kept the names of MFC's
  `COleDateTime` in its helpers, compiled into both servers: a file no distribution's archive could take.
  `Common/Util/DateTime.cpp` replaces it, written from the calendar and the OLE Automation date's definition to the
  old class's behaviour. A differential test held the two to the same answers bit for bit (a NaN's sign aside) under
  MSVC, GCC 15 and clang 21 - every day of the years 0 to 9999, every second of 24 days and some 57 million other
  operations a run, about 84 million comparisons in all - and, on Linux, ran the server's own MIME, IMAP and
  system-date parsers over both; a second check held the new class to Python's calendar on every day of the years 1
  to 9999 and to the C library's local time in twelve time zones. Nothing an administrator or a client sees changes,
  except that a date made from a NaN (which only a COM client could pass) now reads as no date instead of undefined
  behaviour. With it, no file compiled into the server is left whose licence a distribution has to settle:
  `debian/copyright`, the RPM spec and SourcePackages.md no longer list it. A new `datetime-tests` program runs the
  date class's and the date parsers' tests on Linux, where ClassTester does not run, in `linux-build` and in the
  Debian and RPM package builds.
- **`ScriptSite.h` opens with VisionTech's notice again, and says it is altered.** The ATL Active Script host that
  only `hMailServer.exe` compiles (`Common/Scripting/ScriptSite.h`) is VisionTech Limited's of 2001, under a notice
  that may be neither removed nor changed in any source distribution and asks an altered version to say so. Since
  August 2026 `build/add-license-headers.py` had put the project's header above it - its copyright and
  `SPDX-License-Identifier: AGPL-3.0-or-later` - which claimed the file as the project's, and nothing said it was
  altered. The notice is first again, byte for byte as hMailServer's own source had it; below it the file says it
  is altered, by whom, and that the changes are under the project's licence, ending with the project's address and
  `SPDX-License-Identifier: LicenseRef-ScriptSite AND AGPL-3.0-or-later`. The header script leaves the file alone
  (`OWN_NOTICE_FIRST`), and its `--check`, which the checkers job runs, fails when the file opens with the project's
  header again, when its first lines are not the notice word for word (held by the notice's SHA-256), or when it
  loses its own SPDX line. `debian/copyright`'s paragraph for the file names the alterations' holders and licence
  beside VisionTech's ([SourceTarball.md](hmailserver/docs/SourceTarball.md)).
- **A release's download links work, and each file's bundle is beside it at GitLab's permanent address.** 6.3.5's 18
  links were given the path `/downloads/<name>`, and GitLab, which serves a link's path below
  `/-/releases/<tag>/downloads` itself, served each at `.../downloads/downloads/<name>`: a 404, until the links were
  corrected by hand through the API on 29 September 2026. `build/ci/release-chain.py` now gives each link the file's
  name without the version after one slash, and refuses a path under `/downloads/` before anything is signed or
  uploaded. From 6.3.6 every file's path follows that rule - a bundle's and a bundle's `.sigstore.json` copy's too,
  whose paths kept the version up to 6.3.5 (6.3.5's bundles, 6.3.3's copies), as 6.3.4's SBOMs' did - so
  `https://gitlab.com/hmailserver/hmailserver/-/releases/permalink/latest/downloads/hMailServer-x64.exe` and
  `.../hMailServer-x64.exe.cosign.bundle` are the latest installer and the bundle that verifies it, under the names
  the update site's `/download/` gives them. A link's name is still the file's own, which servers and the feed read.
  `release-chain.py --self-test` holds the paths, where GitLab serves them and the pairing, with 6.3.5's paths as a
  negative control; `build/check-release-chain.py` holds them and every `permalink/latest` address the tree writes
  ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), "The links' names and paths").
- **The `.deb` and the `.rpm` require `ca-certificates`**, which every HTTPS request the server makes needs, the
  update check's among them; in the first Linux live update PostgreSQL had pulled it in. So do the source packages
  and the Arch package.
- **The `.rpm` and the RPM spec require glibc's character-set converters**, `glibc-gconv-extra` on Fedora and EL and
  `glibc-gconv-modules-extra` on openSUSE. glibc only recommends them, so every container image of those
  distributions, and any installation made without recommended packages, goes without, and there, with every `.rpm`
  up to 6.3.5, the server read and wrote no text in a character set but Unicode's, US-ASCII, UTF-7, ISO-8859-1,
  ISO-8859-15 and Windows-1252: a header or a body in BIG5, ISO-2022-JP, Windows-1251 or ISO-8859-2 came out empty,
  and a PST's text in code page 932, 936, 949 or KOI8 was imported as its ASCII, each byte outside ASCII a replacement
  character, and an RTF body's read as Windows-1252 (the pinned Fedora 44, UBI 9 and 10, Leap 16.0 and Tumbleweed
  images, 1 October 2026). Every `.rpm` the project makes, the generic ones and each release's own, now requires
  `(glibc-gconv-extra(<arch>) or glibc-gconv-modules-extra(<arch>))`, which each distribution meets with its own
  package, built for the server's architecture: an i686 build of it beside an x86-64 glibc would meet the bare name
  and convert nothing. The spec requires its distribution's one by name. Where the live update installs an `.rpm` with
  `rpm -Uvh` - on openSUSE, which has no `dnf` - nothing is installed beside it, so a server there without the
  converters fails its update to 6.3.6, and stays as it was, until `zypper install glibc-gconv-modules-extra` has run;
  `dnf` installs them with the package. `rpm-install-test.sh` holds each package to the requirement and, in its
  release's clean image, which lacks the converters, requires that the installed server finds every character set it
  names and that BIG5 and ISO-2022-JP text converts, then takes the converters away again and requires that neither
  holds; the RPM source packages' build reads the requirement and their install test requires the first through
  `server-starts`, and `linux-package-rpm` reads the requirement in the generic x86-64 `.rpm`. An AppImage runs on its
  host's glibc and cannot require a package: on Fedora, EL or openSUSE that package is installed by hand
  ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md)), and the server says when it is missing (Operations,
  below).
- **The `.rpm`'s `%changelog` is its own**: one entry, from the version's heading in `CHANGELOG.md` and dated by
  `SOURCE_DATE_EPOCH`, in place of CPack's placeholder of 2010.
- **The Linux update helper runs `apt-get` quietly** (`-q`, `Dpkg::Use-Pty=0`), so the journal no longer holds
  dpkg's progress as "[614B blob data]".
- **The release's own pipeline deploys the update feed with the release in it.** 6.3.5's tag pipeline ended in
  `update-feed`, a trigger of a pipeline on `master` running `pages` alone. GitLab refused it the moment it started,
  once `release-publish` had passed (`invalid_bridge_trigger`, "downstream pipeline trigger definition is invalid"):
  it drops a multi-project trigger naming the project that runs it. The tag's pipeline therefore reads "failed", and
  the feed was deployed by a `master` pipeline started by hand. `update-feed` is gone; in a `v*` tag's pipeline
  `pages` now needs `release-publish` and runs once it has passed, and `package-repos` deploys the site from that
  pipeline. On `master` and in a schedule's pipeline `pages` runs at once, as before. No token, variable or project
  path is involved, so the project's move into the group on 29 September changed nothing here.
  `build/check-release-chain.py` fails on a `pages` that does not wait for `release-publish` and on any `trigger:
  project` in a CI file.
- **The project is at `gitlab.com/hmailserver/hmailserver`.** It moved from the owner's namespace into the group
  `hmailserver` on 29 September 2026, keeping its id, 86730559, and the old addresses redirect. The documents, the
  packages' metadata, the Control Panel's links, the policy link of the server's `/.well-known/security.txt` and the
  Service Desk address name the group's path, and a new database gets `HelpWikiBaseUrl`
  `https://gitlab.com/hmailserver/hmailserver/-/wikis/`; one set up before keeps its value, which GitLab redirects.
  From 6.3.6 every release is signed as
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>` with the project's id, which
  every server from 6.3.4 on trusts. The release scripts no longer sign at the first path, which signed 6.3.5 and
  which servers still trust for it ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **The ARM64 Windows installer is in the release.** From 6.3.6 a release carries `hMailServer-<v>-arm64.exe`, the
  installer for Windows on ARM, beside the x64 one: `release` collects it from `windows-packages-arm64`, held to the
  SHA-256 that job wrote; `release-authenticode` signs it with Authenticode by a second call, and stops if the x64
  installer was not signed; `release-sbom` makes its `.cdx.json` from the signed bytes and `linux-sbom`'s ARM64
  component list; `release-sign` signs it with Sigstore, with its `.sigstore.json` copy, and the provenance names it.
  A release is 60 files and 154 links. A server's update check asks for its own processor's installer by name - as
  6.3.4 and 6.3.5 do - so an x64 server is never offered the ARM64 one, and `/download/hMailServer-arm64.exe` on the
  update site leads to it. The rehearsal on a `batch*` branch collects it too. `build/check-release-chain.py` holds
  every one of those places, and refuses a job the release needs that is allowed to fail, in either of the ways
  GitLab lets it. The ARM64 build is cross-compiled on the x64 Windows runner from libraries built into that runner's
  cache, and has never run on ARM64 hardware; the release notes say so
  ([PackagesWindows.md](hmailserver/docs/PackagesWindows.md), [WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md)).
- **The release notes and the documents check the signer's project id, not only its path.** The commands the
  release's verification section, SECURITY.md, ContainerImage.md and the chart's README gave checked the identity
  cosign verifies, which is a path - one that could pass to another project were the group or the project renamed
  or deleted - and not the project's own id, `86730559`, which the certificate carries and a server from 6.3.4 on
  requires at the group's path. Each now reads it as well: OpenSSL prints the certificate's extension
  `1.3.6.1.4.1.57264.1.15` from a file's bundle, or from each signature of the container image
  (`cosign download signature`), and from the source tarball `build/ci/sigstore-certificate.py repository-id`
  reads it as the server does. `container-image-sign` keeps the certificate Fulcio issued for each of the image's
  signatures (`cosign sign --output-certificate`, `container-image-signed/certificate-<name>.pem`) and fails unless
  `build/ci/sigstore-certificate.sh` reads `86730559` from its extensions; an attestation's certificate, which
  `cosign attest` does not write out, is held to the id by the channels' registry mirror, as before.
  `build/check-release-chain.py` holds the job to the check, with three negative controls.
- **The SURBL lookups and the MX-record test cut host names by the Public Suffix List, and `tlds.txt` is gone.** To
  ask a URI blocklist about a link's host, and to judge a sender's host that has no MX by its domain's, the server
  cut the name to two labels, or to three under a name in `tlds.txt` - SURBL's list of 2006, which stated no licence
  and was installed beside the binary on every platform. It now cuts to the registrable domain by the Public Suffix
  List it already compiles in for DMARC, PRIVATE section included, as SURBL's own list of today does:
  `mail.example.act.edu.au` is asked about as `example.act.edu.au`, not the suffix `act.edu.au`; `a.b.c.kawasaki.jp`
  as `b.c.kawasaki.jp`; `tenant.github.io` as itself, where the old cut gave `github.io` alone; and `mail.gov.je` as
  `gov.je`, one of the 134 names of the old list that are not public suffixes today. SURBL is still asked about the
  whole host too, and a host under a PRIVATE suffix about the domain the ICANN section alone gives as well
  (`github.io`: see "A host under a shared-hosting suffix", under Anti-spam). A sender
  at a public suffix itself that has no MX, `github.io` or `co.uk`, fails the MX-record test, since there is no
  registrant's domain above it to fall back to; the old cut failed the first and passed the second. Error 4335, the
  list not read, is retired. The installer deletes `Bin\tlds.txt` on upgrade, and the Linux packages no longer carry
  it. The cut's tests run in ClassTester and, on Linux, as the new `public-suffix-tests` program, in `linux-build`
  and in the Debian and RPM package builds.

### Anti-spam, anti-virus and mail flow rules

- **A provider can be spared from every auto-ban by its reverse name** (GitLab work item #56; schema 6108,
  `hm_neverbannames`). `AutoBanNeverBan` lists addresses, and a provider whose outbound hosts share a reverse name and
  whose addresses move had nowhere to say "never ban this name". The never-ban list by reverse name holds patterns in
  the domain list's form - `mail.example.net` that name alone, `.example.net` the name and every name beneath it - and
  refuses a public suffix, an address, a network and a `*`. An address is spared from all three auto-bans (failed
  logons, a spam trap, a multi-recipient null sender) - no IP range, no firewall rule, no ban line for fail2ban - when
  one of its reverse names is covered and resolves back to the address (A or AAAA): forward-confirmed, because anybody
  who holds an address can write its reverse name. Nothing is looked up on a session's thread: the SMTP session's own
  reverse lookup feeds the list, and a ban placed before the answer is in is written, resolved on the name lookup
  queue and lifted if the address is spared, before the log line, the alert count or the firewall is told. The failed
  logons counted before that answer - over IMAP, POP3, ManageSieve or REST, which look nothing up until the logon
  that bans - still write their `logon failure` lines, which the packaged fail2ban jail counts on its own: with
  `MaxInvalidLogonAttempts` at five or more, the jail's `maxretry`, fail2ban can ban a listed provider below the
  server. Nothing
  else is skipped, and the list is empty by default. COM (`Settings.AutoBanNeverBanNames`, `AddAutoBanNeverBanName`,
  `RemoveAutoBanNeverBanName`, `CheckAutoBanNeverBanName`), REST (`/api/v1/never-ban-names` and
  `/api/v1/never-ban-names/check`), the Control Panel's Auto-ban page with a check of what the list would decide about
  an address, and the Deck's Lists view; [AutoBan.md](hmailserver/docs/AutoBan.md).
- **Port 25 takes no password: an SMTP listener's AUTH is On, Off or Honeypot** (GitLab work item #54; schema 6108,
  `hm_tcpipports.portsmtpauth`). Port 25 is where other servers deliver mail, and up to 6.3.5 every SMTP listener
  offered `AUTH` there after `EHLO` and checked the password - a spray target on the port every scanner reaches. Each
  SMTP listener now has an SMTP authentication: **On**, as before; **Off**, which does not offer `AUTH`; and
  **Honeypot**, which offers it exactly as On does. On Off and Honeypot an `AUTH` is answered `502 5.5.1 AUTH not
  permitted on this port.` without the account being looked up or the password evaluated - the same answer in the same
  time for every mechanism, user name and password - and the first `AUTH` of a session bans the address for
  `AutoBanMinutes` through the auto-ban's own writer, mirrored to the firewall when `AutoBanFirewall` or
  `AutoBanCommand` is on: one range per address while it lasts, named `Auto-ban: AUTH on a delivery port <address>`,
  however many `AUTH` lines and sessions it sends. Never this computer, an address `AutoBanNeverBan` lists (where an
  incoming relay that sends `AUTH` is named), one the never-ban list by reverse name spares or an authenticated
  session; nothing with `AutoBanMinutes` at 0. One line in the application log names the address, the port, the
  mechanism (as a SASL mechanism name, never what followed it) and the listener's mode; the packaged fail2ban filter
  matches the ban's. Unauthenticated `MAIL FROM` stays inbound delivery. A new installation's port 25 is a Honeypot and
  587 On; an upgraded server's listeners stay On (below). POP3 and IMAP listeners are always On. COM
  (`TCPIPPort.SMTPAuthentication`), REST (`smtp_authentication` on `/api/v1/ports`), the Control Panel's TCP/IP port
  dialog and ports list, the Deck's Ports view, and the setup checklist's *No password on port 25*;
  [SmtpAuthOnDeliveryPorts.md](hmailserver/docs/SmtpAuthOnDeliveryPorts.md).
- **An auto-ban can be widened to its reverse-name neighbours** (GitLab work item #55; `AutoBanWidenByReverseName`,
  off by default). A scanner working through `203.0.113.10`, `.11`, `.12` under one reverse domain earned a ban, a
  firewall rule and a sweep for each. With the switch on, the ban is still written at once for the one address, and is
  then widened in the background, on the name lookup queue, to the contiguous run of addresses around it inside its
  IPv4 `/24` whose reverse names share its registrable domain (the public suffix list's, so
  `mail5.outbound.example.net` is `example.net`): one IP range and one Windows Defender Firewall rule for the run, each
  address of it told to `AutoBanCommand` and the Linux hook, and the before and after in the application log. Only
  when one of the banned address's own reverse names resolves back to it, so a scanner cannot name itself after its
  neighbours; at most 32 lookups each way; never across an address on `AutoBanNeverBan`, one the never-ban list by
  reverse name covers, or one an administrator's IP range tells apart from it or lets in at the ban's priority or
  above; never for IPv6, nor for a network's own address (RFC 1918, RFC 6598, loopback, link-local), whose
  neighbours are its own machines; a lookup not answered leaves the single address. A hook command that runs past
  `ExternalProcessTimeout` is not run again in the same reconciliation, so a run costs one timeout of a hook that hangs,
  not one per address; what it was not run for is not asked again, as a failed run never was. COM
  (`Settings.AutoBanWidenByReverseName`), REST (`auto_ban_widen_by_reverse_name`), the Control Panel's Auto-ban page and
  the Deck's Settings; [AutoBan.md](hmailserver/docs/AutoBan.md).
- **A link's host is the one after its user name.** The SURBL test asked the URI lists about the characters after `://`
  that can be in a name, so `http://anything@evil.example/` was asked about as `anything`, and a spammer escaped every
  list by writing a name and an `@` before the host; a URL entry of the sender lists read the same `anything`. A link is
  now read as RFC 3986 and the browsers read it: the host follows the last `@` before the path, without its port, and is
  percent-decoded (`%65vil.example`); `%40` in the user name is not its end; a bracketed IPv6 address is not asked
  about, since no URI list publishes one, unless it carries an IPv4 address (below); and an IPv4 address in any form a
  browser takes (`0xC0.0.2.1`, `3221225985`) is that address. `http:`, `http:\\` and `http:///` before a host count as
  `http://` does, a backslash ends the host as a slash does, and a link written into another's query
  (`?u=http%3A%2F%2Fx%40evil.example`, `?u=http%3A%5C%5Cevil.example`) is read the same way, however long its run of
  `%2F`, and so is one whose slashes were left as they are (`?u=http%3A//evil.example`, as Python's `quote` and
  JavaScript's `escape` write it). In a text part, where the mail program that shows a link decides where it ends, a
  link it may end before the `@` - Thunderbird ends one at `)`, `"`, `` ` ``, `{` or `}` - is also asked about as the
  name before that character: `http://evil.example)@innocent.example/` is asked about as `innocent.example` and
  `evil.example`. In an HTML part the character references are read as a browser reads them (`x&#64;evil.example`,
  `http&colon;//`), and a quoted attribute's value runs to its closing quote, spaces and a `'` or a `>` in a user name
  included. Punctuation after a link still ends its host; a host with another character in it (`a!b.evil.example`, a
  label in Unicode) is asked about as the name before that character and the labels after its label. Not done: an
  internationalised name is not turned into its `xn--` form, and a host written in fullwidth or other characters a
  browser maps to ASCII is not mapped ([Roadmap3.md](Roadmap3.md), "Internationalised link hosts"); a soft hyphen and
  the other characters a browser deletes are deleted (below). The reading's tests run in ClassTester and, on Linux, in
  `public-suffix-tests`.
- **Characters a browser deletes from a host no longer hide it.** Before it looks a name up, a browser deletes from the
  host the code points UTS 46 ignores - a soft hyphen, a zero-width space, a word joiner, a zero-width no-break space,
  the variation selectors and the rest of the 17 ranges of Unicode's `IdnaMappingTable.txt` 18.0 - and reads its
  percent-encoding as UTF-8, so `http://ev%C2%ADil.example/`, `http://evil<U+200B>.example/` and, in an HTML part,
  `http://ev&shy;il.example/` all go to `evil.example`. The server read a percent-encoded byte past ASCII as U+FFFD and
  deleted nothing, so such a link was no name at all, and a listed name hid behind one character nobody sees. The host
  is now decoded and cleaned as a browser does, in the SURBL test and for the sender lists' URL entries, which are
  stored the same way (`http://sh%C2%ADop.example/` is `shop.example/`); HTML's named references for those characters
  (`&shy;`, `&ZeroWidthSpace;` and twelve more) are read, and a numeric one past U+FFFF (`&#xE0100;`) is the character
  it names, where it was U+FFFD. A zero-width joiner is not one of them: a browser refuses one except after a virama
  (RFC 5892's CONTEXTJ rule, which UTS 46 applies), and no name is read from a host that holds one. In a text part,
  where the mail program that shows a link decides where it ends, a link with a zero-width no-break space in its host is
  also asked about as the name before it: a webmail's script that ends a link at white space by its `\s` ends it there,
  and a browser given the whole link deletes it. So are U+180E and U+200B, which older engines count as white space.
- **A link with no scheme is read.** A browser resolves `<a href="//evil.example/">`, `<img src="\\evil.example/x">` and
  `href="/\evil.example"` against the page they are on, so from a webmail page they go to `evil.example`, and a
  redirector's `?u=//evil.example` (or `?u=%2F%2Fevil.example`) sends its visitor there; none was read. Two slashes or
  backslashes or more are now a link where they open an HTML attribute's value, quoted or not, after the spaces the URL
  parser drops, and where they follow a query parameter's `=`, in a text part as in an HTML part; elsewhere they are a
  path's or a comment's, and nothing is read from them. And in a quoted value the tabs and line breaks the URL parser
  drops are dropped before the slashes after `http:` are skipped: `<a href="http:&#10;//evil.example/">` goes to
  `evil.example`, where the server stopped at the line break and read nothing. A quoted value with a tab or a line break
  anywhere in it is also read without them, as the URL parser reads it, so a link written into its query with one in it
  (`?u=http%3A%2F%2Fev&#9;il.example`) is read as the redirector receives it.
- **A link to an IPv4-mapped IPv6 address is asked about as its IPv4 address.** `http://[::ffff:192.0.2.1]/`, in any
  of the forms the WHATWG IPv6 parser takes (`[::FFFF:C000:201]`), is 192.0.2.1 to a dual-stack system, and
  `http://[64:ff9b::192.0.2.1]/`, under the NAT64 well-known prefix of RFC 6052, is 192.0.2.1 through a translator;
  no bracketed host was asked about, so a listed address escaped every list in brackets. Both are now asked about as
  the IPv4 address, reversed, and a sender list's URL entry for one of them is stored as that address. Any other IPv6
  address, the IPv4-compatible `[::192.0.2.1]` among them, is still not asked about.
- **A host under a shared-hosting suffix is also asked about as the domain the ICANN section registers.** The Public
  Suffix List's cut (under Downloads and the release) asks about `tenant.github.io` as itself, since `github.io` is a
  suffix of the list's PRIVATE section. A list that is wildcarded, as SURBL's multi and the Spamhaus DBL are, answers
  for `github.io` when it is asked about the host; one that is not, as URIBL is not, answers only for the names it
  lists, and a whole hosting domain it lists - `duckdns.org`, say - is listed under that name alone. So a link to
  `evil.duckdns.org` escaped such a listing, which 6.3.5, cutting at two labels, caught. A host under a PRIVATE suffix
  is now also asked about as its registrable domain by the list's ICANN section alone - `duckdns.org`, `blogspot.com`
  for `a.b.blogspot.com` - which the generated `PublicSuffixListData.h` makes possible with the PRIVATE section's rules
  in arrays of their own; DMARC and the MX-record test read both sections as before. The header was regenerated from
  Debian's copy of the list of 25 July 2026, whose rules are byte for byte those compiled in until now.
- **The sender lists' URL entries see every link, and a message is read for them once.** The URL entries were matched
  against the first 16 hosts of a message, the SURBL test's cap, so a message that put the link an entry blocks after
  sixteen others got past it; and the message was parsed and its text read for each recipient with a URL entry, up to
  the MIME parser's 80 MB each time, about 30 seconds per recipient for a crafted message. Every link is now matched,
  and the text is read once for the message against the entries of every scope, however many recipients have one:
  each link is looked up by the names an entry for it could have, so the reading costs one pass over the text and no
  more for more entries, and it keeps one record for each entry host a link is under, whatever the message holds. A
  recipient reached after the list was read again from the database has the links read again. There is no cap on
  the text: a limit on what is read, like the anti-spam size limit that keeps large messages from the SURBL test,
  would let a message put the blocked link after it
  ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **A link to an address is asked about reversed, and only so.** It was asked about in its own order too, which is
  the address the other way round: a link to `192.0.2.1` was tagged when a list had `1.2.0.192`, and a sender
  list's URL entry for `1.2.0.192` blocked or allowed it.
- **A host DNS cannot carry no longer ends the SURBL test.** A host went to the resolver as the message wrote it,
  and the names were asked about in sorted order, so a link at the end of a sentence (`http://example.com.`),
  `http://../` or a label of 64 characters made a name the resolver refuses, and the failed lookup abandoned the
  list for the message with every link sorted after it unchecked. Such a host is not asked about, a name that with
  the list's zone passes 253 characters is not asked, and the links are asked about in the order they come.
- **The URI lists' error codes are never a listing, whatever the expected result.** The Spamhaus DBL answers every
  address with `127.0.1.255`, "IP queries prohibited", and the Spamhaus ZRD with `127.0.2.255`, "IP queries not
  supported", so a server that asked either with no expected result, or with `127.0.1.*` or `127.0.2.*`, tagged
  every message holding a link to an address. SURBL and URIBL answer `127.0.0.1` to a query they block, which tagged
  every message holding a link on a server they block. Those, and Spamhaus's `127.255.255.0/24`, which counted only
  when an expected result took it in, are now errors under any expected result, logged at debug level once for their
  lookup. A list that refuses the server's queries with `127.255.255.x` is not asked again for that message, and the
  application log says so at most every 15 minutes, as for a lookup that fails. `127.0.0.1` does not end the list
  for the message, since Hostkarma answers it for a name on its whitelist and a link to one would switch the list
  off for the links after it; the application log says so, at most every 15 minutes as well, when a list answered it
  to every name of a message.

- **Fixed: a URL entry of the sender lists with an `@` or a `\` in it could name another host.** An entry typed as a
  link was cut at its first `@`, after a leading `www.` had been looked for, so `http://a@b@bad.example/` was stored
  as `b@bad.example/` and `http://user@www.bad.example/` as `www.bad.example/`, neither of which is any link's host,
  and the backslash of `http://good.example\@evil.example/`, which a browser reads as a slash, was not seen, so the
  entry named `evil.example` where the link goes to `good.example`. An entry's host is now read as the SURBL test
  reads a link's (above): the part after the last `@`, up to a port, `/`, `?`, `#` or `\`, percent-decoded, each
  leading `www.` dropped while two labels or more are left (`http://www.www.example.com/` is `example.com/`), and an
  address in any form a browser takes (`0xC0.0.2.61`) written in dotted decimal. An entry whose host no link can have
  - neither an IPv4 address nor a name of two labels or more, of letters, digits, `-` and `_`, within the lengths DNS
  allows and not ending in a number - is refused: an IPv6 address and a name of one label were refused already, and
  the rest (a name with a `!` in it, an empty label, a label of 64 characters) were stored and matched nothing.
  Entries stored before are read the new way when the server loads them, which mends the first two forms; one stored
  from the third names `evil.example` and has to be entered again, and one of a host now refused stays in its list,
  matching nothing as it did. The cases run in `SURBLTester`, in ClassTester and, on Linux, in `public-suffix-tests`.
- **Fixed: a filtering engine's score that is no number could count as the kindest verdict.** The filter hook
  (`FilterHookUrl`) rounds the score an engine answers with to the whole number the spam framework counts in, and for
  a score that is not a number, an infinity or past the range of that number - `NaN`, `Infinity` and `-Infinity` as
  Python's JSON writer spells them, `1e999`, `1e10` - the rounding was undefined: on x64 it gave the lowest score
  there is, so a message the engine had scored as far past every threshold as a score goes was delivered as clean.
  Since 6.2.23 Alpha 1, which brought the filter hook. A score from -1,000,000 to 1,000,000 is taken as it is; any
  other that reads as a number now counts as 1,000,000, the most spam there is, and text that is no number at all is
  still no score. `AntiSpam/FilterHook.cs` holds it to that, on Windows and in the Linux suite.
- **Fixed: a Sieve `size` test whose number and quantifier came to more than 8 EiB answered the wrong way round.** The
  number was multiplied by its `K`, `M` or `G` in 64 bits and wrapped below zero, so `size :over 9999999999G` was
  true of every message and `size :under 9999999999G` of none. The product is now held at the largest 64-bit number,
  as a number written without a quantifier already was. Since 6.2.3, which brought the Sieve interpreter.
  `Sieve/SieveEvaluation.cs` holds it to that.
- **A HELO name or a sender's domain that is not a name can be refused**
  ([work item #51](https://gitlab.com/hmailserver/hmailserver/-/work_items/51)). Two settings, both off by default, so
  nothing changes until one is switched on: `RefuseHeloNotFqdn` answers `550 5.7.1` at `MAIL FROM` to a session whose
  HELO/EHLO name is not a fully qualified domain name, and `RefuseMailFromDomainNotFqdn` to one whose envelope sender's
  domain is not - an address with or without brackets (`192.0.2.7`, `[192.0.2.7]`, an IPv6 literal), a single label
  (`localhost`), a public suffix (`co.uk`, `com`) or anything that is not a host name, while `mail.co.uk` is a name.
  No DNS is asked; the suffixes are the Public Suffix List's, the list the quarantine review groups by. At `MAIL FROM`
  rather than at `HELO`, so AUTH has had its chance; `MAIL FROM:<>` has no domain to judge and is no excuse for a bad
  HELO. Never judged: an authenticated session, an IP range that does not run the spam tests, a sender on the
  anti-spam white list, and an incoming relay, whose HELO is the gateway's own. One log line names the HELO, the
  envelope and the peer. On `Settings.AntiSpam` over COM, `PUT /api/v1/settings/antispam` (`refuse_helo_not_fqdn`,
  `refuse_mail_from_domain_not_fqdn`), the Control Panel's Anti-spam page under Host checks in every language, and
  the Deck's settings; rows of the settings table, so no schema step. docs/SmtpPolicy.md says what a name is here;
  `AntiSpam/FqdnRefusal.cs` and `API/RestApiFqdnRefusal.cs` hold the sessions and the settings to it, and
  `FqdnCheckTester` every rule, in the class tests and in the Linux `public-suffix-tests`.

### Administration

- **Text and values in the Control Panel can be selected and copied (work item 48).** Every page and dialogue: drag
  over text, double-click a word, Ctrl+C, or right-click for Copy / Select all; with nothing selected, Copy takes the
  whole text, and a selection dragged out into another program carries plain text only. Text inside buttons, lists,
  tabs, menus, grids and editors is left to its control, so a press there stays a press, and keyboard navigation and
  screen-reader output are unchanged. The words of the controls themselves are copied whole: a right-click on a check
  box, a radio button, a button or a toggle, a drop-down, a tab, a page in the navigation tree or a label offers Copy,
  which copies the caption as it is shown - a drop-down's value as it shows it, without an access key's underscore or
  an icon - and Ctrl+C copies the same on the one that has the keyboard, unless a data grid or a list around it would
  copy its chosen rows, which it still does. A left click and every key those controls take do what they did, an
  icon-only button offers no menu, and a control with a menu of its own keeps it; a drop-down's right-click menu,
  which offered Copy, Cut and Paste and could run none of them, now offers Copy. Most of Logging's and API &
  monitoring's settings are check boxes, whose captions could not be copied before. Every plain list - domains,
  accounts, aliases and distribution lists, public folders, incoming relays, an account's IMAP folders, folder
  permissions, group members, recipients, a route's addresses and the Active Directory picker - copies its selected
  rows with Ctrl+C or its right-click Copy, one row per line in the list's order, and offers Select all where several
  rows can be chosen; the Ctrl+K search's results copy from their right-click Copy (its search box keeps the
  keyboard); data grids copy their rows as before. Live logs selects several lines (click, Shift+click, Ctrl+A) and
  copies them exactly, in the log's order; it follows new lines only while the end is in view and no longer splits a
  line the server is still writing into two rows. On REST API keys, a key's label, id, state and scope can be copied.
  Every Copy button now writes the clipboard the same way: a program that reads the clipboard the moment it changes
  (Windows' clipboard history, a remote-desktop client) no longer makes a copy fail after a second's freeze (a chart's
  Copy then showed no confirmation, and the new API key's and app password's Copy an error), and text copied just
  before the Control Panel is closed still pastes after it has gone.
- **Where the keyboard is shows in one violet, in the Control Panel, the Control Deck and the webmail.** A text field
  that has the keyboard - clicked or tabbed into alike - takes a focus colour for its border and a soft glow of it
  outside, the look of Firefox's address bar: in the Control Panel every text box, password box, number box, combo box
  and date picker with a border round it, the search boxes among them and the fields of every dialog; in the Control
  Deck and the webmail every input, drop-down and text area with a box of its own and the search boxes, and in the
  pages the server serves on its own - the password-protected message, account recovery, a confidential message, a
  shared file's password, a list's subscription page. A field drawn as a line - the Control Panel's search palette,
  the webmail's compose window and command palette - takes the colour in its line. The ring the keyboard leaves on a
  button, a link or a row takes the same colour in the Deck and the webmail, as the navigation's keyboard border does
  in the Control Panel; on the compose window's dark header it is the dark theme's violet, and inside a calendar event
  filled with its calendar's colour it keeps a band of the page between the two. The violet is #7542E5 on the light
  themes and #B89CFF on the dark ones, and each clears 3:1 against every surface a field sits on and beside its glow
  at the glow's strongest (WCAG 2.2, non-text contrast). The glow is drawn outside the field and moves nothing; under
  Windows High Contrast and a browser's forced colours the system's own colours stay, the web pages giving a field an
  outline in the system's highlight; the brief fade-in stops when animations are turned off. Tested by
  `DesignTokensTests` (the contrast, High Contrast, the web pages' copies of the colours) and by `deck-script-test.js`
  and `portal-script-test.js` (each page's tokens and rules).
- **Fixed: the webmail's Sign in button was wider than its fields.** On the sign-in card the button ran 32 pixels
  past the right edge of the E-mail address and Password fields, and *Send the link* past its field on the reset card,
  in every browser and both themes: the buttons' `all: unset` put back `box-sizing: content-box`, so a width of 100%
  had their padding added to it. They are as wide as the fields now (measured in headless Chromium and Firefox, light
  and dark), and `build/check-portal-script.py` refuses a control reset with `all: unset` that is given a width of
  100% without saying `box-sizing: border-box` again. The Deck's sign-in card and the password-protected message's
  page were not affected.
- **Fixed: two copies of work item 48 that were not yet the Control Panel's own.** Text copied from the help pane
  put RTF and XAML on the clipboard beside the text, in the theme's colours, and its right-click menu was the
  document viewer's, in English (measured in German and Swedish); Ctrl+C, the menu's Copy and a selection dragged
  out now carry plain text, written the same way as every other Copy, and the menu is Copy / Select all in the
  Control Panel's language. A right-click on a result of the Ctrl+K search, for its Copy, left the keyboard on the
  result rather than in the search box, so typing reached nothing until the box was clicked again; the search box
  now keeps the keyboard, and Copy still copies the result that was right-clicked.
- **Fixed: words of the Control Panel that stayed English, or lost a space, in its sixteen other languages.** The
  app password's "Copied.", a list editor's "Deleted.", directory synchronisation's "Applying...", the "in" before
  each page's place in the Ctrl+K search's results, a dashboard chart's time axis ("last 3 minutes") and what its
  legend says of each line to a screen reader and in a tool tip, the LDAP page's "Could not write" and the anti-virus
  overview's "and" between the two notices were English in every language: the localisation check took one word
  ending a sentence for a host name and did not look at a piece of a sentence joined to others, and it now reports
  both. 290 translations in sixteen languages had lost the space at their end that the program joins the next
  words to, so a Danish copy that failed said "Kunne ikke kopiere:" with the reason run on, and a status line ran
  two sentences together; the spaces are back, and the check holds every translation to the white space at the
  edges of its English (Japanese and Chinese may drop it after a character of their own script). Two Finnish
  counts had a space too many, and four sentences were split around a value with their end left in the source,
  where no translator could move it: an account's renamed address told German "Die Adresse muss ein Name gefolgt
  von @ seinexample.com.", and a group's name opened with « in French and Russian and closed with an English
  quote. The four are whole sentences now, and the check reports a space a translation adds and a sentence
  split that way. A dashboard chart's "collecting" said "fetching" in six languages. And the words WPF puts on
  the screen itself - Copy, Cut and Paste on a text box's right-click menu, a scroll bar's right-click menu - were
  English in Danish, Finnish, Norwegian, Dutch, Swedish and Ukrainian, which .NET has no translation for, and
  WPF's German scroll bar menu named both Top and Page Up "Nach oben"; in the Control Panel's seventeen languages
  they are now its own words, and in English and in a language it has no catalogue for they stay WPF's. The two
  buttons that page a date picker's calendar were "Previous" and "Next" to a screen reader in every language and
  every view, the theme's English; they now say what they page as the calendar shows months, years or decades
  ("Previous month", "Next decade"), in the Control Panel's language.
- **Fixed: the Control Panel window's own buttons had no name for a screen reader.** Minimise, Maximise (Restore
  down while the window is maximised) and Close in the title bar were each read as just "button", in every
  language; they now carry their names, in the Control Panel's language. So do the chevron that folds each of the
  navigation's nine groups ("Collapse Maintenance", "Expand Maintenance") and a password box's eye ("Show password",
  "Hide password"), each as its state changes; both were nameless in every version of the Control Panel. A chart's
  time axis says "last second" where it said "last 1 seconds".
- **Fixed: a switch put back from the audit trail stayed as it was.** Putting a setting back from its audit row
  (`POST /api/v1/audit/{id}/revert`, `Utilities.RevertAuditedChange`, *Put back* in the Deck and the Control Panel)
  wrote a switch's old value, `true` or `false`, into the text column of `hm_settings`, which the server does not
  read, and answered that it was put back. A setting now goes back into the column the server reads it from - a
  switch as a switch, recorded as `false -> true` - and a text whose values hold ` -> ` is put back whole, where it
  was cut at its first arrow, while its row is the setting's latest change in the trail, or refused when the row does
  not say where the old value ends. A switch put back by 6.3.4 or 6.3.5 keeps a `true` or `false` in its unread text
  column, which does nothing; one put back twice by those versions left a row `true -> false` or `false -> true`
  describing that text column, and putting that row back now sets the switch. `Utilities.RevertAuditedChange`'s help
  text in the type library says so too.

- **Fixed: `Utilities.IsStrongPassword` knew three of its special characters wrongly on Windows.** A password of
  five or six characters counts as strong only with a digit or one of a set of special characters, and three of
  them - the currency sign, the diaeresis and the acute accent (U+00A4, U+00A8, U+00B4), upstream's, from a Swedish
  keyboard - had been written over with U+FFFD when the source was re-encoded in 2014. The Windows build, which
  reads that source in the ANSI code page, took each U+FFFD for the three characters its UTF-8 bytes are in
  Windows-1252 (U+00EF, U+00BF, U+00BD), so a five-character password ending in a currency sign was weak and one
  ending in an i diaeresis strong; the Linux build took U+FFFD itself. The three are back, written as `\u` escapes,
  which every compiler reads alike. Nothing in the server calls the method, which answers COM scripts only; a
  password an administrator chooses is judged by the password policy. The passwords the server generates, twelve
  hexadecimal digits, are longer than the rule looks at.
- **The audit trail's put-back names every refusal.** `POST /api/v1/audit/{id}/revert`'s description in the OpenAPI
  document, `Utilities.RevertAuditedChange`'s comment in the type library's source and the audit trail's help page
  listed the refusals of 29 September. They now also name, where each was missing, a setting's row whose detail does
  not hold the two values, a `[Settings]` value the store cannot hold, a write that fails, and the two refused with
  the sentence every other way of changing them is refused with: one of the five `[Settings]` keys of the live
  update's trust, which only `hMailServer.INI` itself changes since 6.3.6, and a setting the server's environment
  sets - each unless the value put back is the one it has already. The help string in the type library is
  unchanged: it says that the call raises why it cannot, within the 260 characters MIDL keeps.
- **Fixed: an account saved through the object COM's `Authenticate` hands out could lose its retention.**
  `Application.Authenticate` and `AuthenticateWithCode` hand the caller a copy of the account signed in, and a
  webmail's password or out-of-office plugin changes that copy and calls `Save()`. From 6.2.25, which brought the
  account's `MessageRetentionDays`, to 6.3.5 the copy was made without it: the copy's `MessageRetentionDays` held
  whatever its memory happened to hold, and the save wrote that over the account's own. So an account's retention -
  a number of days, -1 to keep its mail for ever, or 0 to follow its domain - could become another number once its
  holder changed a password or an out-of-office reply through such a plugin, and the retention sweep, every six
  hours, then deleted the account's delivered mail older than the number the save left, or kept it, or followed
  the domain, where the account had said otherwise. Windows only: the Linux server has no COM. The copy now
  carries every field of the account, the mailbox key wrap a password change prepares on it among them. After
  upgrading, check the retention of the accounts whose holders use such a plugin (the account's page in the Control
  Panel, COM `Account.MessageRetentionDays`, REST `message_retention_days`). `AccountTester`, which the self-test
  and the Linux program `business-object-tests` run, copies an account with every field set and compares each
  one, and `MessageRetentionTests.TheAccountAuthenticateHandsOutKeepsItsRetentionThroughASave` saves the account
  COM hands out and reads the retention back.
- **Fixed: `POST /api/v1/ipranges` took priorities its update refuses.** Since 6.2.25 the create read `priority` as
  any run of digits, with `atol`, where `PUT /api/v1/ipranges/{id}` takes a whole number from -2,000,000,000 to
  2,000,000,000: a number beyond that was stored as given while a 32-bit integer holds it, and past that Windows
  stored the integer's bound and Linux handed the database a value its column cannot hold; a fraction was cut at its
  point; a priority written as a string (`"5"`) was passed over, and the range made with priority 0; and the digits
  were looked for after the first `"priority"` anywhere in the body, so a range named `priority` could be made with 0
  whatever its priority said. The create now finds the member by name, as it finds the range's name and addresses,
  takes what the update takes, and answers anything else with 400 "priority must be a whole number between
  -2000000000 and 2000000000", making no range; a body without a priority, or with `null` there, still makes one of
  priority 0. The OpenAPI document says so. Tested by `RestApiCoverage.IpRangesRoundTrip` and
  `business-object-tests`.
- **The audit trail records somebody's mail read through an administrator's route.** An account's folder tree, a
  folder's permissions, every message, a folder's messages, a message's header fields and its source under
  `/api/v1/accounts/{address}`, a backup's listing of a mailbox, a mailbox exported to the server's disk or downloaded
  as `export.zip`, a held message's headers and text, the archive index searched or one of its entries read, and a
  page of an eDiscovery search's hits, are each a row - object type `account` and action `mailbox-read`,
  `quarantined message` and `read`, `archive` and `searched`, `archive entry` and `read`, or `discovery search` and
  `read` - under the credential that read them and the address it came from; a support session opened as an account
  is a row, `support-session-opened`. A row names a message or a folder by its id, a held message or an archive entry
  by its sender and recipients, an archive search by what was asked for and an eDiscovery search by its id and its
  case, never by a subject or a folder's name. The same read by the same credential from the same address is one row
  a minute, so that the Deck's *Shared folders* page and a client paging a listing do not bury the trail; another
  message, folder or credential is a row at once, and an export or a support session is a row every time - an export
  that stopped part of the way too, with how far it got. A read the server refuses is not recorded. A read over COM
  or over IMAP by the master user is not recorded, nor what a support session does once open, which the application
  log holds request by request ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md), "Mail read through an
  administrator's route").

- **`-Insecure` works on Windows PowerShell 5.1, and `hmctl --version` says the release's version.** The
  `HmailServer` module's `-Insecure`, which accepts the self-signed certificate of a server on the loopback, had no
  effect on Windows PowerShell 5.1, which has no `-SkipCertificateCheck`. There it now puts a certificate check in the
  process's place for that call alone, one that accepts the certificate of the session's loopback address and port
  and hands every other connection to the check it replaced, and puts that back when the call ends; on PowerShell 7 an
  `-Insecure` call follows no redirect, since the skipped check would follow it. `Invoke-HmApi` refuses `-Insecure`
  for a host other than the loopback as `Connect-HmServer` does. Against a loopback server with a self-signed
  certificate, on 5.1 and 7.5, 14 of 14 cases pass, where the module as it was failed three on 5.1 and two on 7.
  `hmctl --version` said `hmctl 1.0`, and its requests `hmctl/1.0`, whatever the release; it says the server's
  version now, stamped with the release (RELEASE.md, step 7), and `build/check-cli-packages.py` fails `checkers` while
  `hmctl`'s `VERSION` is not `Version.h`'s, with a `--self-test` case the check as it was passed.

### Operations

- **Fixed: on MySQL and MariaDB an insert refused as a duplicate could be read as one that wrote a row.** A caller
  that ignores a duplicate - two nodes recording one complaint report or DMARC or TLS report, a greylisting triplet
  written by two connections at once - tells a written row from a refused one by the id it is handed back. After a
  refused statement `MySQLConnection` asked the client library for the insert id anyway, which MySQL documents as
  undefined after an error and which libmariadb answers with the id of the last row the pooled connection wrote, in
  whatever table (measured against MariaDB 11: a duplicate refused with 1062 after an insert into another table read
  as that table's id), so a duplicate report was recorded as new. It hands back none after a failure it was told to
  ignore, as the other three backends do.
- **Fixed: the lines `[Database] UpgradeOnStart` writes at the service's start were dropped.** Upgrading.md promises
  a line in the application log for each step; they were written before the service had read its settings, when its
  logger writes nothing. They are kept and written once the settings are loaded, and so are the counts schema 6107's
  prerequisite reports when DBUpdater runs it over COM, which DBUpdater's reinitialise at the end of the upgrade now
  writes ([Greylisting.md](hmailserver/docs/Greylisting.md), "What the upgrade to 6107 does").
- **Fixed: on Linux, some of the server's network waits had no timeout.** The MTA-STS policy fetch, the DANE TLSA
  query, the OAuth2 token request (`OutboundOAuth2TokenUrl`), the ACME client and its http-01 responder, and the
  ManageSieve and metrics listeners set their socket timeouts in the form Windows takes, which Linux refuses, and the
  three HTTPS requests among them read with calls that on Linux wait without a timeout in any case. So on Linux a
  wait on a peer that stopped answering lasted for as long as the connection stayed open. This was so in every
  release with Linux packages, 6.3.0 to 6.3.5; Windows honoured the timeouts, each of which bounded one read. The
  three HTTPS requests now go through the server's one HTTPS client, whose deadline bounds each request whole on
  both platforms - 15 seconds for the MTA-STS policy, 20 for the token, 30 for an ACME request - and the others
  take their timeouts in the form each platform takes: 5 seconds for the TLSA query, the http-01 responder and the
  metrics listener, 30 for ManageSieve. What this changes besides is in What changes on upgrade, item 15. A new
  tester program the Linux build runs, `network-deadline-tests`, holds the token and ACME requests, the TLSA query
  and the http-01 responder to their deadlines against a peer on 127.0.0.1 that never answers, and the policy fetch
  to its own through a proxy of its own that never answers, and fails against the code as it was; `HealthProbes`,
  `ManageSieve` and `OutboundXOAuth2` in the regression suite have a case each.

- **The ACME client reads a header that comes last.** `AcmeClient` looked a response header up in the block cut
  at the blank line and then wanted a line break after its value, so a `Replay-Nonce` or `Location` that a CA sent
  as the last header was read as absent. HTTP gives the order of fields no meaning (RFC 9110, section 5.3). A
  `Replay-Nonce` sent last left the client without a nonce, and every certificate request stopped at its first
  step; a `Location` sent last left the account's and the order's URLs empty. The last header now runs to the end
  of the block, as the migration client's lookup already had it, and the self-test and `network-deadline-tests`
  pin it. Found in the SAST triage of 6.3.6, in the function of a cppcheck finding (AcmeClient.cpp:208) that was
  itself a false positive.

- **Why the container image waits, said truly.** `.gitlab/ci/container-image.yml`,
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) ("What the owner sets on the registry", 3),
  Roadmap2's release-chain row, the packaging README and a comment of `build/check-release-chain.py` said, and
  6.3.4's notes below say, that the image waits because GitLab.com cannot transfer a project whose container
  registry holds an image. GitLab's documentation does not say that: on GitLab.com a project whose registry is
  enabled is transferred only within its top-level group, one with more than 1,000 container repositories not at
  all, and a project must hold no image only on GitLab Self-Managed. The image waits because the maintainers decided
  on 27 September 2026 to move the project into the group `hmailserver` before the first image is pushed, so that
  its address, `registry.gitlab.com/hmailserver/hmailserver`, is the lasting one - a container repository's path is
  always its project's - and 6.3.4's `SigstoreVerifier` already trusts the group's path with the project's id,
  86730559. The texts that said the image appears at `registry.gitlab.com/progressiverobot/hmailserver` - the
  comments of the Compose file, the chart's values and the Dockerfile, the packaging README, EvidencePack.md,
  ContinuousIntegration.md's `cosign verify` example and SECURITY.md's `cosign verify-attestation` example, and
  `renovate.json`, which also said the registry was not switched on - now name the group's address; the Compose file
  and the chart pull from it since the move. Nothing the pipeline does changes.

- **The documents say what the pipelines have run.** `batch41`'s pipelines of 27 September 2026 ran the package jobs
  for the first time, and [PackagesDeb.md](hmailserver/docs/PackagesDeb.md),
  [PackagesRpm.md](hmailserver/docs/PackagesRpm.md), [PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md) and
  [PackagesWindows.md](hmailserver/docs/PackagesWindows.md) now say which, job by job, where they said no job had
  run: every `.deb` build job passed in pipelines 2886955494 and 2887115157, and every `.rpm` build job in
  2887115157; no install job had started in any pipeline, no AppImage job had been started, and
  `windows-packages-x64` built the unsigned x64 installer in 2887115157.
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md) no longer says no GitLab job has run, says
  the GitHub workflows stayed unchanged from `faca023d4` until `6593259e0` removed them, and gives the registry's
  cleanup policy as the API does: it kept two of the five patterns the document listed, and the maintainers added the
  other three through the API on 28 September 2026, before the policy is ever switched on; it and README.md say that the fuzz schedule and Renovate's schedule and token
  exist since 27 September. README.md no longer says the project has no ARM machine to build the AArch64 packages on
  (the pipeline cross-compiles them), `build/ci/release-chain.py` no longer says the pipeline does not build them,
  and Roadmap2's Windows on ARM row names the CI job it said there was not.
  [PackageManagers.md](hmailserver/docs/PackageManagers.md) no longer says the GitHub workflow built the manifests
  for the releases up to 6.3.3 - it arrived after the `v6.3.3` tag, and no release ran it - nor that the AppImage is
  attached to each release, nor that the Chocolatey identifier `hmailserver` is free: it holds upstream's
  hMailServer 5.6.8.2574, unlisted, and publishing under it needs the project to become one of its maintainers; its
  table no longer says the workflow ran on every release, and its walk for the first Chocolatey push says the push
  waits for that. The documents that sent a reader to `6399e08a5` for the removed GitHub workflows now give
  `faca023d4`, the same commit as `master`'s history holds it: `6399e08a5` is its copy from before its branch was
  rebased, which `master`'s history does not hold.

- **`HM_DB_WAIT_SECONDS`** (0 to 999999, default 0): the image's entrypoint waits for the `[Database]` server to
  accept a TCP connection before opening it, and does not start (exit 69) when it has not. The entrypoint reads the
  ini's `[Database]` section as the server does (spaces trimmed, names in any case), and `HM_INSTALL_KIND`, which
  the image sets for the update check, is not a reason for it to open the database.

- **A build without the live update, and a live update that knows a snap and a container**.
  CMake's new `-DHM_LIVE_UPDATE=OFF` builds the server with no apply step and installs neither
  `hmailserver-update` nor its two units; an AppImage made from an OFF tree carries no `hmailserver-update`
  either. Its update check still names a newer release and the distribution's own command
  (`-DHM_UPDATE_COMMAND="apt upgrade hmailserver"`). A server in a snap (`SNAP` naming the directory it runs from)
  or in the container image (`HM_INSTALL_KIND=container`) names `snap refresh hmailserver` or the image to pull,
  and never downloads or installs a release file. `GET /api/v1/update` reports `installKind`, `liveUpdate`,
  `updateCommand` and `updateAdvice`, and the Control Deck shows them. What the live update did in images before
  6.3.6 is written down in [LiveUpdateKinds.md](hmailserver/docs/LiveUpdateKinds.md): it installed nothing,
  downloaded two packages and reported HM6890. Tested by `build/test-install-kind.sh` (CI job
  `linux-live-update`).

- **A new Windows installation can be set up against SQL Server, PostgreSQL or MySQL with no window** (not yet
  run against a server: that is the installer smoke test's `--database` run, made by hand, not by a pipeline).
  Both installers take the database on the command line - `/dbtype=`, `/dbhost=`,
  `/dbport=`, `/dbname=`, `/dbuser=`, `/dbpasswordfile=`, `/createdatabase=`, `/dbservice=` - or in the
  container's own `HM_DB_*` variables, the password only in `HM_DB_PASSWORD` or a file, never on a command line.
  `DBSetup.exe /unattended` then creates the database, or uses one that exists and upgrades it with DBUpdater if
  it is older, and ends only at the schema the build requires, its lines in the setup log. Settings it cannot use
  are refused before anything changes, among them a password file that is not UTF-8, a control character, and a host
  or user wrapped in quotation marks, which `hMailServer.ini` would take off; a quote, a backslash or a semicolon in
  a password is passed on as it is (below, "Database passwords"). This is how a silent installation of the ARM64
  build, which has no built-in database, finishes; its installer now refuses a silent new installation given no
  database before anything is installed, rather than open DBSetup's wizard where nobody can answer it. On x64 it is
  an option beside the built-in database, which stays the silent default unless `HM_DB_TYPE` is set in the machine's
  or the user's environment, which makes a new installation an external one; the setup log says whether the type came
  from `/dbtype` or the environment. The x64 installer still does not run on ARM64 under emulation (`x64compatible`),
  by the maintainers' decision of 28 September 2026, which WindowsOnArm.md records with its reasons. The installer
  smoke test checks the path with `-DatabaseSettings` (`installer-smoke-run.sh --database`).

- **Scripting off runs no script engine.** With scripting off the server no longer compiles the event script and runs
  it once per handler at every start, reinitialise and reload; switching scripting on - by COM, REST or an
  environment - loads it, compiled and searched, before the switch takes effect, and reports then a script that does
  not compile, whatever runs beside the switch - a reload, a reinitialise, a switch-off, another switch-on: a reload
  that read the switch as off is made again once it is on, and of two loads that overlap, the script of the one
  started last stays in force, unless it does not compile, which is reported. A rule's Run function action against a
  script loaded while scripting was off, which never compiled, is refused (HM5018) instead of being handed to the
  engine. An empty script runs no engine.

- **Crash records that place the fault.** The crash oracle's record adds, after the fields it always had, what an
  access violation read, wrote or executed and where, the faulting address's image base and offset, and up to twelve
  frames of the faulting thread; the REST API writes to the application log which method and path it answered 500
  after an exception.

- **Fixed: settings read while they were being reloaded.** hMailServer.ini's database and directory values, the
  administrator's second factor, and each value of the settings store and its map are read and written under locks,
  each value whole; a reinitialise or a COM Settings.Directories call could hand another thread an empty or freed
  value. A reinitialise now builds the settings store, the SMTP, POP3, IMAP and cache configurations, the server
  messages, blocked attachments, SSL certificates, DNS blacklists, SURBL servers and the local addresses whole
  before another thread can see them, and hands each over under a lock: a COM call made during one - the Control
  Panel's or a script's - could copy a pointer while it was being replaced, or find the settings not yet read and
  take every one as missing (0 or empty, and HM5015 in the ERROR log). The logger's level, device and format, and
  whether the live log takes entries, are read and written whole (`live-log-tests`, which `linux-build` runs, and
  under ThreadSanitizer), and `[Database] UpgradeOnStart` reads as off until the file is first read. Several values read
  one after another are still not read as one set, and a setting changed during a reinitialise, after the new
  settings were read, is stored but not in force until the next reinitialise or start.
- **Fixed: a list or a manager COM handed out before a reinitialise was not the server's after it.** Each reinitialise
  made new lists - the incoming relays, routes and remote domain policies, the public folders and groups, the server
  messages, blocked attachments and SSL certificates, the DNS blacklists and SURBL servers - so a COM object for one
  of them taken before it, as the Control Panel keeps one while its page is open, held the old list: a route added, a
  relay deleted or a message changed through it reached the database and the old list, and the server went on without
  the route, trusting the deleted relay and sending the old text until the next reinitialise or start. The lists are
  now made at the start and read again in place by every reinitialise, each read whole under its own lock, so every
  COM object for one holds the list the server reads. `Database.IsConnected` and `EnsurePrerequisites` answer for the
  database the server is connected to, where an object made before a reinitialise asked the connection manager the
  reinitialise had closed, and a `BackupManager` object asks for the backup manager in force at each call, so that a
  backup started through an old one no longer runs beside one started through the new; the two managers are replaced
  under a lock, where COM, the SQL log device's worker and the other threads a reinitialise does not stop could copy
  the pointer while it was being replaced, and a restore asked for through a `Backup` object in the moment between the
  old manager and the new is refused with a sentence, where it reached no manager at all and was answered as an
  unknown error. Tested in the Windows suite (`SettingsDuringReinitialize`: lists and a Database object held across a
  reinitialise).
- **Fixed: a profile verb an event script asked for while a profile was being applied or reverted.** An apply or a
  revert of a setting profile writes settings while it holds the profiles' lock, and a write can run an event script
  on the same thread: an error reported fires `OnError`, and a revert whose way back holds the scripting switch runs
  the script's top level. A script that then asked over COM to apply, revert, discard or decline a profile locked the
  mutex its own thread held, which the Windows build answered with an unknown error and a POSIX build would never
  have finished. Such a call is now refused at once, with the reason, and the apply or the revert goes on
  (`profiles-tests`, and `RestApiProfiles` in the Windows suite, where a revert that switches scripting back on runs
  a script that asks for a discard).
- **Active-active nodes: the design, not built** ([ActiveActive.md](hmailserver/docs/ActiveActive.md)). Until it is,
  one service per database.
- **Fixed: languages read while they were being reloaded.** A reinitialise rebuilt, in place, the translations COM's
  `Languages` and `Language` objects read - a script's or another COM client's; the Control Panel has translations of
  its own - and a language's first `String` call read its file into a table other calls were reading at the same
  time, which could crash the service on Windows (on Linux nothing reads them). Both are now built whole and handed
  over under a lock, as the settings are since the reinitialise fix. A language reads its translation from the
  directory its English was read from, where one taken before a reinitialise that changed `ProgramFolder` read the new
  directory's file through the old directory's English, whose numbered strings need not agree (`language-tests`, and
  the class tests on Windows).
- **Fixed: a part of the server that two threads asked for first at the same moment could be made twice.** Each of
  the server's 61 kinds of shared part made on first use (the logger, the caches, the rate limiter, the recipient
  callout's cache, the outgoing OAuth2 token client and the others) was checked for outside its lock and not again
  inside it, so two threads arriving together - two deliveries, or a COM call beside a REST request - could each make
  one, and what the first was given was lost; a thread could also be handed one before it was whole. Each is now made
  exactly once, and, as before, never taken down at exit; `singleton-tests`, which `linux-build` runs, holds it to
  that.
- **Fixed, on Linux: a rewrite of `hMailServer.ini` as root could give the file an owner nobody chose.** A rewrite
  goes to a temporary file that is given the owner, group and mode of the file it replaces, and the owner was taken
  from a look at that file whether or not the look had found it. When the file had gone between being read and being
  rewritten - removed or renamed by something else at that moment - the new one written as root, as
  `sudo hmailserver --set-admin-password` writes it, was given an owner and group no one had set, and the service
  could lose its own configuration. Since 6.3.0, the first Linux release. Such a file is now made as a file that was
  not there is, the writer's own and readable by it alone (0600); a file that is there keeps its owner, group and mode
  as before. `ini-file-tests`, which `linux-build` runs as root, holds it to that, with a race against another user's
  process that removes the file and makes it again while root rewrites it.
- **Fixed: a calendar edit at the largest SEQUENCE made the event older than it was.** The REST routes of the
  calendar and the tasks (the webmail's) write SEQUENCE one higher when an event with attendees or a task is changed,
  and at 2147483647, the largest an int holds, adding one wrapped to -2147483648, so a calendar that held the event
  took the change for an older version than its own. It now stays at 2147483647, and a SEQUENCE past that reads as
  2147483647 on Linux as on Windows: Linux read its low 32 bits, 4294967297 as 1. A task's PRIORITY that another
  client wrote outside 0 to 9 is listed as 0, none, since 0 to 9 is all the routes take; and a `priority` or an
  `alarm_minutes` past an int's range sent to them is refused with 400 rather than converted first, which was
  undefined - an alarm of `1e20` minutes was taken as no alarm. An alarm whose TRIGGER, in an event a calendar
  client stored or in an invitation the webmail shows, comes to more minutes than an int holds is listed as
  2147483647, held to its end as SEQUENCE is, where its digits and their sum overflowed. Since 6.3.4, which brought
  the routes. `API/RestApiCalendar.cs` holds it to that.
- **SCRAM holds a stored password hash's iteration count to the bound a password logon holds it to.** SCRAM-SHA-256
  read a stored PBKDF2 hash's count with no upper bound - one past an int as 2147483647 on Windows and as its low 32
  bits on Linux - and asked the client for that many rounds; the server never writes such a hash. One that says more
  than 10,000,000 is now refused, as a password logon refuses it, and the exchange fails at the proof, as for an
  unknown account. Since 6.2.0, which brought SCRAM. A password logon on Linux read the same count as its low 32
  bits too, and so took 4294967297 as one round; it now reads it as SCRAM does, and the two refuse the same hashes
  (since 6.3.0, the first Linux release). `IMAP/Basics.cs` holds SCRAM to that on Windows, and the tester program
  `scram-tests` both on Linux.
- **Fixed: a COM look-up that found nothing left an object behind.** Asking a COM collection for an index, id or name
  it does not hold - `Languages.Item`, `Domains.ItemByName`, `Accounts.ItemByAddress` and 62 other look-ups,
  `IMAPFolders.Add` when the name was taken or the folder could not be saved, and `Application.BackupManager` when no
  backup manager was available - made the object it would have answered with before looking, and kept it, never
  released, when the answer was no. Each now looks first. `Recipients.Item` and `DiagnosticResults.Item` also refuse
  a negative index, which read outside their list.
- **The legacy Blowfish scheme of stored secrets is the project's own code.** `Common/Util/LegacyBlowfish.cpp`,
  written from Bruce Schneier's published description of the cipher, replaces `BlowFish.cpp`/`.h`/`.h2` (Schneier's
  reference code made a C++ class by Jim Conger, whose files stated no licence, which neither Debian nor Fedora
  ships). Every value the scheme holds - an `hMailServer.ini` database password with `PasswordEncryption=1`, a secret
  stored while `ProtectStoredSecretsWithDPAPI` is 0, the passwords and tokens a backup or a person's export carries,
  and COM `Utilities.BlowfishEncrypt` and `BlowfishDecrypt` - is written and read byte for byte as 6.3.4 did: about
  31 million comparisons with the old code and 60,000 answers from a 6.3.4 service found no difference. HM7277's
  source is now `LegacyBlowfish::EncryptToString`.
- **COM `Utilities`'s helpers that do work answer an authenticated caller or an event script.** Through
  `Application.Utilities` or created as `hMailServer.Utilities`, `BlowfishEncrypt`, `BlowfishDecrypt`,
  `ResolveMXRecords`, `GetMailServer`, `IsLocalHost`, `CheckSieveSyntax`, `EvaluateSieveScript` and `CriteriaMatch`
  answer a caller that has called `Authenticate` (an account of any level, or the administrator) or an event script
  running in the service, as the rest of the COM API and the equivalent REST routes do; a server with no
  administrator password answers as it did. `MD5`, `SHA256`, `GenerateGUID`, `IsStrongPassword` and the three syntax
  validators stay open.
- **The last update's outcome survives the check that follows the start.** The service reads the update helper's
  outcome at start-up, and on 6.3.4 and 6.3.5 the start-up check, a second later, replaced the whole update status:
  after an update `Status.UpdateApplyOutcome`, `apply` in `GET /api/v1/update` and the Control Panel said nothing,
  and only the log had it (issue #10's test, 29 September 2026). A check now keeps it; it stays until the service
  stops or the next outcome replaces it. Tested by `UpdateSchedule.TheLastApplysOutcomeOutlivesTheStartUpCheck`.
- **An update check that gets no answer says why, and a failed scheduled check is tried again soon after, once.** A
  new server's first check failed with "The response carried no HTTP headers." after 10.8 seconds, and the next with
  the scheduled check alone was a day away. An answer that ends before its headers is now reported with who ended the
  connection, after how many bytes and how many seconds after the connection was made, the TLS handshake's share and
  how the read ended - for every request of the server's HTTP client, not only the update check (not the installer's
  and bundle's downloads) - and a scheduled check that fails when the one before it did not is due again ten minutes
  later and read at the quarter-hourly task's next run, a quarter of an hour after a failed scheduled check, then
  every `UpdateCheckHours` as before. `GET /api/v1/update` gains `nextCheck`, when the scheduled check is next due.
  Tested by `UpdateCheck.AFeedThatEndsTheConnectionWithoutAnAnswerIsReportedAsSuch` and
  `UpdateCheck.AScheduledCheckThatFailedIsTriedAgainOnceTenMinutesLater`.
- **A verified installer is kept across a restart.** With `UpdateAutoDownload=1`, a service that cannot install an
  update itself (`NT SERVICE\hMailServer`, `HM7463`) downloaded the whole installer, 88 MB for 6.3.5, every time it
  started. A download now finds the installer and bundle an earlier one left in the `Updates` folder, holds them to
  everything a fresh download is held to, and fetches nothing when they pass; a copy that no longer passes is deleted
  and fetched again, and a start whose first check fails no longer sweeps the kept copy away. Tested by
  `UpdateSchedule.AVerifiedInstallerIsKeptAcrossARestartAndNotFetchedAgain` and
  `UpdateSchedule.AKeptInstallerOutlivesAStartWhoseFirstCheckFails`.
- **Fixed: a backup destination inside the data directory failed every backup, and so every unattended update.**
  The copy of the data directory walked into its own staging folder until the path was too long (`HM5014`): the
  first Linux live update, whose destination was under `/var/lib/hmailserver`, was refused for an hour and a half.
  A backup now leaves out, and a restore leaves in place, the destination, the temp directory and the run's staging
  folder - and a restore the folder of the archive it restores from - wherever they are inside the data directory,
  recognised by the directory itself rather than its spelling; the backup log names each. Such a destination works,
  and the backup is then on the disk it backs up ([Backup.md](hmailserver/docs/Backup.md)). The same change removes
  two faults of the Linux packages, whose temp directory is inside the data directory, found by reading the code and
  not reproduced: an encrypted backup with messages failed the same way, and a restore of messages deleted the store
  it had just extracted. Tested by `backup-tests`, `BackupExecuterTester` and
  `BackupRestore.ADestinationInsideTheDataDirectoryIsBackedUpToAndRestoredFrom`.
- **A backup no longer carries the live update's folder.** `Updates` - the package or installer waiting to be
  applied, the rollback image, the apply token and the last apply's outcome - was in every archive, 15 MB of the
  first Linux live update's 15.4 MB, and a restored copy of the outcome would have a server report an apply it
  never made. It is left out as the recoverable store is, and a restore leaves this machine's own where it is.
- **The live update's trust is read from `hMailServer.ini` alone.** The server read `UpdateSigningIdentity`,
  `UpdateSigningIssuer`, `UpdateSourceRepository`, `UpdateTrustRootsFile` and `UpdateLogPublicKeyFile` from the
  database, where the REST API and the Deck write, and root's `hmailserver --verify-update` on Linux from the file,
  so a signer set over REST was used by the one and not the other. The process an update is checked for must not
  choose what it is checked against: on every platform the server now reads them from the file, and COM and the REST
  API (409) - and so the Control Panel, the Deck, a profile and the audit trail's put-back - refuse a change. **On
  upgrade** a value 6.3.4 or 6.3.5 stored in the database no longer applies: the first start takes it out and names
  it once as `HM5809`. Set them with `sudo hmailserver --set-setting` on Linux, and in the file on Windows
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "Where the trust is read from"). `hmctl config export` and
  `Export-HmConfiguration` leave the five out, as they leave out what the server's environment sets, and `diff` and
  `apply` of a document an earlier version exported skip them with a line naming them rather than fail the apply
  ([CommandLine.md](hmailserver/docs/CommandLine.md)). Tested by `trust-anchor-tests`, by `IniSettingsOverCom`,
  `RestApiSettings` and `IniSettingsMirror`, and by `check-cli.py`.
- **The backup and event logs are UTF-8 on Linux**, as the other logs are; they were UTF-16LE. A file an earlier
  version wrote is converted when the server first opens it. Windows keeps UTF-16LE.
- **Fixed: a message whose last bytes the disk refused was kept short, and taken for whole.** A write to a file goes
  into a buffer first, and closing the file writes the last of it - 4 KB on most systems - so the close is the only
  report of whether a file's end arrived. The server did not check it, in every version up to 6.3.5, so when a disk
  filled or a quota was reached just as a message's last bytes were written, the message was kept without them and
  nothing said so: SMTP answered 250 to DATA and to BDAT, IMAP APPEND answered OK, and a message collected from an
  external POP3 or IMAP account was delivered short and then deleted from the remote server, where the account
  deletes what it collects. A header the server writes into a message after accepting it - the `Return-Path` and
  `Delivered-To` of local delivery, a DKIM signature, an ARC seal, the `Return-Path` SpamAssassin is shown, the
  sandbox's verdict - could put a short copy in place of the whole message, and so could the message store's own
  writes (a compressed or encrypted message, a copy of one, an import), a journaling copy, a copy encrypted at the
  gateway, an IMAP migration's copy and every file the server writes whole through `FileUtilities`.
  `MessageStoreFsync`, off by default, had the spool files of SMTP and of the fetchers and the store's own writes
  checked before their close; nothing else was. Every writer that takes a file for whole now takes the close's answer
  too: SMTP answers `451 4.3.0` and the sender tries again, with `HM5862` in the error log once per message; APPEND
  answers NO (`HM5211`); a collected message stays on the remote server for the next collection (`HM5862`, then
  `HM5864` for POP3 or `HM5865` for IMAP); a header rewrite leaves the message as it was, and the delivery goes on as
  it did when the rewrite could not be written; an IMAP migration tries the run again. Tested by
  `file-utilities-tests`, against `/dev/full`, a quota and a full disk, and by `SpoolWriteFailure` for DATA, BDAT,
  APPEND and both kinds of collection.
- **Fixed: a SpamAssassin result the disk refused part-way cut the message short.** With `SAMoveVsCopy` at its
  default, the server copied SpamAssassin's result onto the message file, and a copy onto a file empties it before it
  writes it: when the disk filled during the copy, the message was left short or empty, in every version up to 6.3.5,
  and the server went on with it as if it were whole. The result is now copied beside the message and renamed over it
  once it is whole; a copy that fails leaves the message as it came, delivered without SpamAssassin's headers, as
  when spamd cannot be reached (`HM5861`). A result whose file did not close cleanly is not applied either. Tested by
  `file-utilities-tests` (`FileUtilities::CopyOver`, against a quota and a full disk). With `SAMoveVsCopy=1` the
  result was moved onto the message instead, which is a copy onto it when the temporary folder and the data folder
  are on two volumes; it is now moved beside the message and renamed over it in the same way
  (`FileUtilities::MoveOver`), and a move whose copy fails leaves the message as it came (`HM5860`).
- **Fixed: two more files whose last bytes went unchecked.** The list of members a backup restore hands 7-Zip, and a
  file the live update downloads (the installer and its Sigstore bundle), were taken for whole when their close
  failed, in every version that has them: a restore could extract fewer
  members than it asked for and go on, and the live update could take a short installer or Sigstore bundle for
  whole until its check refused it. Both now take the close's answer:
  the restore stops with the list's file removed, and the download fails, says that the disk may be full and leaves
  nothing behind. Tested by `file-utilities-tests`, with a loopback server for the download, against a quota and a
  full disk, and each beside its control where the disk takes it.
- **The object store's committer says what ends a pass.** Since 6.3.4, which brought the object store, a pass that
  met an unexpected error ended without a word. Nothing was lost, and nothing is: what it had not put in waits on the
  local disk, is read from there, and the next pass takes it up. It now says so in the error log as `HM7473`, once an
  hour at most ([ObjectStorage.md](hmailserver/docs/ObjectStorage.md)).
- **On Linux the server says which character sets the machine cannot convert.** It converts mail's and IMAP's text
  with the C library's iconv, which loads a character set's converter only when one is asked for, and a converter
  that was not installed made the text empty and said nothing. `hmailserver --check-config` now ends with a line
  `Character sets:` that says every character set the server converts text in - the code page table's 24 and the
  PST import's - converts here, or names those that do not, with the package that has them on Fedora, EL and
  openSUSE; and a start with any missing writes the same to standard error, which the journal, OpenRC's console log
  or an AppImage's terminal keeps, and to the application log. A warning, not a refusal: mail in the other
  character sets is served, and `--check-config` still exits 0, as the OpenRC service's start needs. The checks of
  the `.rpm` above read that line (`Unicode::CodePageCharacterSets`, `Unicode::ConvertsCharacterSet`,
  `platform/main.cpp`; the packaging guide, [step 5](hmailserver/source/Server/platform/packaging/README.md)).
- **The auto-ban hook is run from where the build installed it, and on NixOS the auto-ban reaches the firewall.**
  With `AutoBanFirewall=1` the Linux server runs `autoban-hook`, which keeps an nftables set of the banned
  addresses. It ran `/usr/lib/hmailserver/autoban-hook` whatever prefix it was built for, while the build installs
  the hook below its own prefix: the packages', `/usr`, had it, and NixOS, where the flake's package is in the Nix
  store, and a build installed under `/usr/local` did not, so there the setting found no hook (`HM6523`).
  `CMakeLists.txt` now compiles the hook's installed path into the server (`HM_AUTOBAN_HOOK`,
  `AutoBanFirewall::PackagedHook`), and refuses a prefix the server could not run as one command; every package's
  path is the same as before. The NixOS module's new `autoBanFirewall` stores the setting and gives the service
  `CAP_NET_ADMIN` and `nft`, which the hook needs - the packaged unit leaves that capability to a drop-in, and the
  option is that deliberate step - and does so too when `settings` sets `AutoBanFirewall` to 1; every program the
  service starts then has the capability, whatever `AutoBanCommand` names among them. Neither a link at
  `/usr/lib/hmailserver` on NixOS nor the hook's path as a setting: the first writes outside the store, and the
  second would be one more program the REST API could name for the server to run with that capability. The NixOS
  test places a ban through the REST API and reads it in the set, and the ban deleted, out of it; with KVM it
  passed, and with the server as it was it failed there ([Nix.md](hmailserver/docs/Nix.md), "The auto-ban below
  the server").

- **Fixed: sixteen more COM look-ups that found nothing left an object behind.** This release makes 67 look-ups look
  before they made the object they answer with; sixteen written as an if and an else were not among them, and still
  made it first and kept it, never released, when the answer was no: `Aliases.ItemByDBID` and `ItemByName`,
  `DistributionListRecipients.Item` and `ItemByDBID`, `IMAPFolderPermissions.ItemByDBID` and `ItemByName`,
  `IncomingRelays.Item`, `ItemByDBID` and `ItemByName`, `Messages.Item` and `ItemByDBID`, `RouteAddresses.ItemByDBID`,
  `Routes.Item`, and `SecurityRanges.Item`, `ItemByDBID` and `ItemByName`. Each now looks first. A sweep of every COM
  method that makes an object, for a return that neither hands it over nor releases it, finds none left.
- **The compromised-account detection and the rooms' booking records say what an unexpected error stopped.** Since
  6.3.4, which brought both, seven catch-alls around a whole pass or operation dropped every unexpected error without
  a word - on Windows, where the server is built with `/EHa`, an access violation among them: the detection's
  handling of a verdict (the baseline, the lock and the alert), its unlock, its discarding of an account's held
  messages when the account is deleted, its reading of the locks at start and its sweep; and a room's forgetting of
  one booking's record and of the records whose calendars are gone. Each still stops there, as it did, and now
  writes one line to the error log saying what was not done and what happens next: `HM6811` for the verdict,
  `HM6812` for the sweep, `HM6813` for the locks, `HM7561` for the unlock, `HM7562` for the discarding with a
  deleted account, and `HM7434` for the booking records
  ([CompromisedAccounts.md](hmailserver/docs/CompromisedAccounts.md), "In the error log";
  [RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md)).

- **Fixed: every TLS context switched on every OpenSSL option from bit 32 up.** The mask of options the server sets
  on each TLS context (`SslContextInitializer`) was held in an `int`. OpenSSL's `SSL_OP_ALL` carries bit 31, so the
  value was negative, and OpenSSL's 64-bit option word received it with bits 32 to 63 set. Every listener (SMTP,
  POP3, IMAP, ManageSieve, the metrics and REST listeners, web services) and every outgoing connection set up by the
  same code (SMTP delivery, the external POP3 accounts' fetch, the SpamAssassin client, the HTTPS client, ACME, the
  IMAP migration client, the syslog TLS sink) had them, in every release linking an OpenSSL that names options
  there: on Windows from 6.0.0-B1 to 6.3.5 (OpenSSL 4.0), on Linux from 6.3.0 wherever the distribution's OpenSSL is
  3.2 or later. What could be seen on the wire: on Windows every outgoing TLS 1.3 handshake carried an Encrypted
  Client Hello GREASE extension, and TLS 1.2 handshakes in both directions offered the compressed elliptic-curve
  point formats RFC 8422 deprecates, as they did on Linux with OpenSSL 3.6 or later; on Linux with OpenSSL 3.2 or
  later certificate compression (RFC 8879) was off in both directions. None of it lowered the protection of a
  connection as the server sets it up. `SSL_OP_PREFER_NO_DHE_KEX`, among the bits, acts only with
  `SSL_OP_ALLOW_NO_DHE_KEX`, which the server does not set, so TLS 1.3 resumption kept its (EC)DHE exchange - except
  on a host whose OpenSSL configuration file allows resumption without one (`Options = AllowNoDHEKEX`, which no
  distribution sets by default), where the server preferred that for a client offering both, and a session resumed
  so had no forward secrecy. The other options there act only on what the server does not use (kernel TLS,
  Encrypted Client Hello keys), and a bit no OpenSSL names yet would have switched on whatever option a later OpenSSL
  gave it. The mask is a `uint64_t` now and holds only what the server asks for. Certificate compression stays off
  in both directions, on purpose: it keeps a decompressor that the peer feeds out of the handshake, and saves a mail
  server nothing it needs. The two HTTPS clients this code does not set up, the MTA-STS policy fetch and the OAuth2
  token request, keep it off now too: on Linux with OpenSSL 3.2 or later they offered to take the web server's
  certificate compressed. A new tester program the Linux build and the source packages run, `tls-options-tests`,
  sets up a listener's and a client's context for every combination of the TLS versions and the cipher-order
  settings, and the two HTTPS clients' context, and reads back with `SSL_CTX_get_options` what OpenSSL holds; it
  fails against the code as it was. `SslTlsVersionTests` has a case that reads the EC point formats a listener
  offers.

- **Fixed: six failures that stopped a pass or a step were said nowhere.** Each is still caught where it was, so
  nothing else stops with it, and is now written to the error log, once an hour at most: a pass of the migration
  scheduler that starts and resumes the jobs (`HM7240`); the filing of a delivered calendar reply, cancellation or
  invitation into the recipient's calendar (`HM7555`, new; the message is delivered all the same); the opening of an
  encrypted mailbox's key at a sign-in (`HM7366`, once an hour an account; the sign-in stands); the recording of a
  message trace event (`HM7554`, new; the delivery goes on and the trace has a gap); keeping an account's list of
  devices to its fifty (`HM6991`); and the SNMP agent's refresh of the delivery queue's two figures (`HM7422`).
  Until now a migration that did not start, replies that were not filed, a key that did not open, a trace with
  gaps, a device list past its limit or two SNMP figures that stopped moving left no cause in any log - the trace
  since it came, in 6.2.22's pre-releases, and the other five since 6.3.4. Migration.md, Calendar.md,
  EncryptionAtRest.md, Reports.md and Snmp.md name the codes. A seventh, the sandbox poller's reading of its own
  interval, is said too (`HM7560`, new, Sandbox.md; the poller goes on every 30 seconds until the interval can be
  read): it was swallowed without a word since 6.3.4.
- **Database passwords are passed on as they are.** The server wrote the host, the user, the password and the database
  into its connection string as they were: for PostgreSQL between single quotes, so a quote or a backslash in a
  password ended its value early; for SQL Server ending at a semicolon, and a value that began with a quote was read
  as a quoted one or did not parse ("Format of the initialization string does not conform to the OLE DB
  specification"). PostgreSQL's values are now quoted and escaped as libpq reads them, with the root certificate's
  path in the code page libpq opens it by. SQL Server's user and password are handed to ADO's `Open` beside the
  string, not in it, and its server and database are in the quotes ADO reads, the server as `Data Source`, OLE DB's
  own name for it, where it was the provider's `Server`; a name with a double quote beside a single quote or a
  semicolon, which no string ADO reads can carry, and a failover partner with a quote, a semicolon, a brace or an
  equals sign, are refused with the reason. MySQL takes each value as an argument of its own, but the server named the
  database bare in two statements of every connection, so a database whose name MySQL takes only in quotes - with a
  hyphen in it, say - could not be used; it is quoted now. A PostgreSQL or MySQL value with a character outside ASCII
  reached the database server as nothing on Linux, and on Windows so did one with a character beyond U+00FF, the
  client then connecting with its own defaults: such a value now goes in UTF-8. On Windows a value whose characters
  are all U+00FF or below still goes in Latin-1, as it always did there, so that a password with such a letter that
  connects today, set on a PostgreSQL server whose encoding is WIN1252 or LATIN1, goes on connecting; such a password
  on a server in UTF8 is still refused there, as it was (PostgreSQL 17 initialised each way takes only the bytes of
  its own encoding). So DBSetup no longer refuses a quote, a backslash or a semicolon
  ([WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md), "What the server passes on"; `DBSetup.Tests`), and the
  container's entrypoint refuses (exit 64), before `hMailServer.ini` is written, an `HM_DB_*` value the file would not
  give back: a line break or another control character, white space at either end, or quotation marks around it; a
  quote or a backslash inside is written as it is. Tested by `connection-string-tests` (every value libpq reads back
  as given, nothing else moved; the bytes each value is handed over in, and MySQL's quoted database name), by the
  class tests on Windows (the same, and ADO's own reading of SQL Server's string), by
  `build/test-docker-entrypoint.sh` in the `update-helper` job, and against PostgreSQL 17, MariaDB 11.4, MySQL 8.4 and
  SQL Server 2022 with such values on 1 October 2026.
- **The live update requires the project's id at the path the project began at too.** A certificate for
  `https://gitlab.com/Progressiverobot/hmailserver`, where 6.3.5 was signed, is now refused without the project's id,
  86730559, as one for the group's path always was, and no setting takes one without it: servers 6.3.4 and 6.3.5
  checked the id there only when the certificate carried one, because no GitLab certificate had been seen when the
  entry was written. 6.3.5's records it, so it installs, and rolls back to, as before; what is refused is what a
  project that is not this one could sign at the path the project has left, which a personal namespace hands to
  someone else when its owner is renamed. The release chain's own checks of a release's files, provenance and images
  (`release-files.py`, `release-chain.py`, `registry-mirror.py`, `container-image-deb.sh`) hold both paths to the id
  likewise ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md), "What a server built after 6.3.3 checks").

### Build and tests

- **The static-analysis findings of the network code, fixed.** Twenty-two findings of the `static-analysis` job are
  gone from its baseline (`build/ci/static-analysis-baseline.txt`), two of them on GitLab's Security dashboard: the
  seventeen `dangerousTypeCast` findings on the sockets' timeouts and `SO_REUSEADDR`, which were the two defects under
  Operations and What changes on upgrade, item 15 - the socket timeouts are now set through `AcceptLoop::SetTimeouts`
  or in each platform's own form, and `SO_REUSEADDR` through `AcceptLoop::AllowRebindPastTimeWait`, on POSIX only; the
  two `danglingTemporaryLifetime` findings (High) in `AcmeClient.cpp` and `OutboundOAuth2TokenClient.cpp`, whose code
  the fixes replaced, though the lifetime they named was sound, since the string class copies; a `cert-err34-c`
  `atoi` in the ACME client's own status-line parser, which went with it; the empty catch around the start of a
  connection the REST and web-services listeners have accepted (`HttpServer::Accept_`), which now says in the
  application log that a connection was dropped, for the first and then once per thousand with the count; and a
  `bugprone-unused-return-value` in `HTTPClient.cpp`, a plain-HTTP client that nothing in either build called,
  deleted with the finding. Nothing new is added to the baseline. The new tester program, `network-deadline-tests`,
  runs in `linux-build` and in the Alpine and Nix package builds, which run every program whose name ends in
  `-tests`; the source packages' builds, which refuse every socket, leave it out.

- **[Roadmap3.md](Roadmap3.md): the plan for everything the project had on GitHub.** A new file at the
  repository root maps, row by row, what it takes to bring back on GitLab what a user could download, verify and
  install before 18 September 2026 - the AArch64 `.deb` and `.rpm`, the AppImages, the container image, the
  Sigstore bundles and the build provenance - and to add what the owner asked for beside them: the ARM64 Windows
  installer, the apt and dnf repositories, winget and Chocolatey, and every other channel people install mail
  servers from. It opens with a counter line like Roadmap2.md's, and `build/check-roadmap2.py` now holds both
  files: it fails when either counter disagrees with its rows or either file is missing, checks one file alone
  when given its name, and has a `--self-test`, run by the `checkers` job, whose controls include a wrong
  Roadmap3.md counter beside a right Roadmap2.md one, which the check as it was let through. README.md,
  Roadmap.md, CONTRIBUTING.md, SUPPORT.md, the documentation index and the Feature and merge request templates
  link it. The `.deb` and `.rpm` help pages, which said no Roadmap2 row carried choosing a package by
  distribution, now name the Roadmap3.md row that does.

- **Ten static-analysis findings of numbers held to their range, fixed; `ini-file-tests` and `scram-tests`.** The
  fixes are the filter hook's score and the Sieve quantifier (Anti-spam, above) and the configuration file's owner,
  the calendar's SEQUENCE and PRIORITY and the iteration count SCRAM and a password logon read (Operations, above);
  with them, a POP3 `LIST` sizes its buffer in `size_t`, and the POP3 session's destructor calls its own
  `OnDisconnect` by name, which is the call it made. Their ten lines are gone from
  `build/ci/static-analysis-baseline.txt`. `ini-file-tests`
  (`platform/ini_file_tests_main.cpp`) is a tester program `linux-build.sh` runs with the others: a file another user
  owns keeps its owner, group and mode when root rewrites it; a file read and gone before it is written - the server
  reads a FIFO whose writer removes it before closing it, so the moment is not left to chance - is made as the
  writer's own, 0600; one made again by another user in that moment gives the replacement that user's owner and mode;
  and two thousand rewrites while another user's process removes and remakes the file each leave it root's own, the
  other user's or absent. The cases that need root say they are skipped without it. `scram-tests`
  (`platform/scram_tests_main.cpp`) holds the rounds SCRAM asks a client for to a stored hash's count from 1 to
  10,000,000 and the unknown account's 210000 past it, as the regression suite's case does on Windows through COM's
  `Database`, which the Linux suite cannot reach, and a password logon to refusing the same hashes.
- **A clock flake in the two-factor tests, not a server fault.** The Windows gate of pipeline 2887524317 failed
  `TwoFactorAuthentication.ACodeComputedElsewhereIsAccepted` with "The previous step must be accepted": the test
  began at 23:37:29.5 UTC, half a second before a thirty-second step ended, computed the previous step's code in
  step s and had it judged in step s+1, two steps behind the server and outside its window of one step either side.
  `OneTimeCodes.WaitForRoomInTheStep` now returns at once when ten seconds or more of the step remain and otherwise
  sleeps into the next step, and the two tests that judge codes against the server's step
  (`ACodeComputedElsewhereIsAccepted` and `ACodeIsTakenOnceOverCom`) call it after enrolling. The suite's other
  codes come from `OneTimeCodes.Fresh`, which never hands out a step behind the clock, or are refused whatever the
  step (a wrong code, one five steps out, a wrong password or a spent budget), so they needed nothing. The new
  fixture `ServerFreeTests.OneTimeCodesStep` replays the failed run against a clock of its own, without a server,
  in both suites: without the wait the previous step's code is two steps behind (its negative control), with it
  the code is judged in its own step.

- **The ARM64 toolchain check could not find the ARM64 C++ runtime on any machine.**
  `build/ci/packages/check-arm64-toolchain.ps1` looked for it with `Get-ChildItem -Path '...\VC\Redist\MSVC\*\arm64'
  -Filter 'Microsoft.VC*.CRT'`, which with a wildcard in the path returns the `arm64` folders themselves and filters
  their names. It reported the runtime missing on the laptop, whose Build Tools 18.9 has it, and on bench150 in
  pipeline 2887115157, where `windows-packages-arm64` stopped on that report with the ARM64 tools and ATL for ARM64
  present, as 6.3.5's notes below say. The check now asks for `...\arm64\Microsoft.VC*.CRT` as one path. It also no
  longer tells the owner to add components the instance already has: when every component is installed and a file is
  not there, it prints the Installer's `repair` of the instance, or, for the runtime, the `modify --add` of the
  component that the instance's own catalog says installs `Microsoft.VisualCpp.CRT.Redist.ARM64` when the instance
  lacks it. `check-arm64-toolchain.ps1 -SelfTest` holds both to instance trees and catalogs made to order, with the
  old path and the old command as its negative controls; on the laptop the check now exits 0.

- **The command-line packages are checked wherever they change.** `build/check-cli-packages.py`, in the
  `checkers` job, fails when the manifest's version or function list drifts from `Version.h` or the module, or the
  Python package gains a dependency, a version of its own, an install hook, a second copy of `hmctl` or a
  standard-library module newer than Python 3.8, or `hmctl` runs code when it is imported; its `--self-test` breaks
  each rule in a copy. The new `cli-packages` and `cli-powershell` jobs (`.gitlab/ci/cli-packages.yml`; merge
  requests and branches, never a tag) build the sdist and the wheel twice and compare them, run `twine check
  --strict`, install the wheel alone, and run `Test-ModuleManifest` and PSScriptAnalyzer - from the PowerShell
  Gallery, held to its published SHA-512 - under pwsh. PSScriptAnalyzer's 25 findings in `HmailServer.psm1` are
  fixed or justified on the one function, with no command, parameter or confirmation prompt changed;
  `Export-HmConfiguration -Verbose` now says why the ini section is empty when the server does not answer for it.

- **The Linux build includes the distribution's `zlib.h`.** Six core sources included the zlib header vendored
  for `hMailServer.exe` on every platform, so the Linux server was compiled against zlib 1.3.2's header and
  linked against the distribution's library (1.3.1 on Ubuntu 26.04). They include `<zlib.h>` under
  `HM_PLATFORM_POSIX` now; the Windows build compiles the same text as before.

- **An ARM64 publish of the .NET tools no longer takes the x64 COM wrapper from an earlier x64 publish.** MSBuild
  searched each tool's own `publish` folder before the reference's hint path, so `build-tools.ps1 -Platform ARM64`
  after an x64 publish compiled all five tools against the x64 wrapper (warning CS8012) and shipped it, in the
  add-ons' `Addons` folders where nothing replaced it; `check-pe-machine.ps1` found five. The publish folders are
  kept out of the projects' items (`Tools\Directory.Build.props`). CI builds in a clean checkout and never met it.

- **`native-dependencies.ps1 -LibrariesRoot`** holds each linked library's version in an installer's component
  list to the tree the build used, which records its own (OpenSSL's `VERSION.dat`, Boost's `version.hpp`,
  PostgreSQL's `meson.build`); the ARM64 list was checked that way against the ARM64 libraries of 26 September.
  The lists themselves are unchanged.

- **OpenSSL 4.0.3 in the Windows builds, x64 and ARM64** (Security, above). `hMailServer.vcxproj`'s include and
  library paths, `post-build.bat`, `build.ps1`'s ARM64 copy and both installer scripts name `openssl-4.0.3`, and
  `libraries/build-openssl.ps1` pins its archive's SHA-256, `325b5c80...16d9`: the value in Debian's signed
  `openssl_4.0.3-1.dsc`, the archive's own OpenSSL signature checked against the key openssl-library.org names, since
  OpenSSL publishes its `.sha256` files on GitHub only. libpq moves from PostgreSQL 18.3 to 18.6 with it, built
  against 4.0.3 (`postgresql-18.6`, `-arm64` on ARM64, in the same paths and the same installer lines), its archive held
  to `983ee554...ff3f`, the `.sha256` ftp.postgresql.org publishes beside it. Of the fixes PostgreSQL made from 18.4 to
  18.6, the one in libpq itself, CVE-2026-6477 (18.4), is in the large-object functions, which the server never calls;
  the others are in the server, its contrib modules and its command-line programs, none of which the installer
  carries. The DLL keeps its name, `libpq.dll`.
  `Get-LibraryVersions.ps1`, `native-dependencies.ps1` and `dependency-scan` read the version from the project file,
  so the SBOMs and the scan name 4.0.3 with no change of their own.

- **A merge request answers for what it brings.** On GitLab's shared runners, with no secret:
  `build/check-binary-manifest.py`, run by `checkers` in every pipeline, fails on a committed binary that
  `hmailserver/docs/third-party-binaries.json` does not list at its SHA-256 (what `verify-binary-provenance.yml` did
  until 18 September); `semgrep-sast-gate` holds a merge request on a Critical or High `semgrep-sast` finding in a
  file it changes that `.gitlab/security-exceptions.yml`, as the merge request's base has it, does not accept
  (`build/ci/security-gate.py --merge-request`, the release's own rule; OpenSSF Baseline OSPS-VM-06.02); and
  `mr-dependency-check` holds one that adds a dependency version with a Critical or High advisory in OSV's own
  export of NuGet's or PyPI's advisories (`build/ci/mr-dependency-check.py`), what GitHub's dependency review did
  (`.gitlab/ci/mr-checks.yml`; ContinuousIntegration.md, "Merge request checks").

- **The server is built and tested with the newest GCC as well as clang.** `linux-build-gcc` (`.gitlab/ci/gcc.yml`;
  linix; `master` and `batch*`, never a tag) builds the server and its tester programs with the CI image's own GCC,
  Ubuntu 26.04's GCC 15, and runs them and the OpenPGP round trip; `build/ci/linux-build.sh` takes
  `HM_COMPILER=gcc`, and a GCC build makes no packages. The Debian and Ubuntu package jobs already built with GCC
  12, 13 and 14; none built natively with GCC 15. The CI image already carried it through `build-essential`, and is
  unchanged.

- **OpenSSF Scorecard, self-run.** `scorecard` (`.gitlab/ci/scorecard.yml`, the schedule described `scorecard`) runs
  Scorecard 5.5.0 taken from cache.nixos.org by its Nix store path, with every path of its closure pinned by NAR
  hash, not from GitHub, and `build/ci/scorecard-pages.py` publishes its result and a badge at
  https://updates.progressiverobot.com/scorecard/; a result it cannot fetch or read never stops the site's
  deployment. Until the owner's `GITLAB_AUTH_TOKEN` exists it says "not configured" and passes.

- **SonarQube Cloud's scope, and GitHub kept out of a job.** `.sonarcloud.properties` scopes SonarQube Cloud's
  automatic analysis to the project's own code and tests. `.gitlab/ci/no-github.yml`'s `.no-github` puts GitHub's
  six hosts at 0.0.0.0 and `::` inside a job, has the hosts file read first where an image has no
  `/etc/nsswitch.conf`, and refuses a job it cannot protect. ContinuousIntegration.md says what GitLab's free tier
  and Ultimate give and why the Code Quality widget is not used.

- **The channels are checked where they change.** `channels-check` runs every channel script's self-test and proves
  the verification with the pinned cosign itself against `build/ci/channels/fake-sigstore.py`, a Sigstore of the
  jobs' own (a Fulcio-shaped authority with an embedded SCT, a Rekor-shaped log, a trusted root).
  `build/check-channels.py`, in `checkers`, holds the jobs to protected master, by hand, allowed to fail, no ID
  tokens and `.no-github` first, and the PKGBUILD, the AUR's pinned host keys, OBS's `_service` and the Copr Makefile
  to shape.

- **The Windows gate is not failed by an earlier run's crash record, and keeps what places a fault.**
  build/ci/windows-gate.ps1 moves aside and prints the crash-oracle.log the binary before it left, and on a failure
  saves the service's module list with every recorded address placed in it (server-modules.txt) and the Application
  log's crash events (crash-events.txt). On 28 September a gate failed its first test on the record a killed earlier
  gate had left, with every other test passed.

- **Boost for the package builds comes from the job's cache, archives.boost.io or SourceForge.**
  build/ci/packages/fetch-boost.sh makes six attempts at each source and holds every copy to the pinned SHA-256; the
  EL, openSUSE and Debian 12 build jobs cache the tarball under its SHA-256. On 28 September three package jobs of
  6.3.5's pipeline failed because some of archives.boost.io's addresses did not answer the runner.

- **The CI's Python is 3.14, and Renovate no longer proposes a locked requirements file it cannot lock.** Every
  Python job runs in python:3.14-slim-trixie; the locked requirement files are remade for 3.14 (the next entry
  says which versions moved). Renovate's Python merge request of 28 September 2026 had moved twelve versions inside
  build/ci/requirements.txt without resolving or hashing them - click kept its old hashes, and Semgrep 1.178.0
  accepts none of the new versions - so the semgrep job, and with it the release's security gate, would have
  failed. Its pip manager now reads the .in files only, and its merge request says how to remake the locked files.
  Microsoft.NET.Test.Sdk is 18.10.1.

- **The rest of what Renovate proposed, taken where it holds, and `renovate.json` held to what is chosen.**
  Taken: every job image that is not a build target at its tag's digest of 28 September 2026, those of the seven
  jobs added after Renovate's merge request among them (`postgres:16`, Microsoft's .NET SDK image), and GitLab's
  Container Scanning analyzer at its tag's digest of 30 September, since GitLab had stopped serving the one pinned;
  uvicorn 0.54.0 and Playwright 1.63.0, the locked files remade with uv; the sixteen updates of the `packages.config`
  test projects (Azure.Core 1.63.0, MSAL 4.90.1, OpenTelemetry 1.19.1, the Microsoft.Testing packages 2.4.1, NLog
  6.2.1 and the rest); NUnit 5.0.0 in RegressionTests, StressTest and LinuxRegressionTests, which nunit3-console
  3.22.0 and NUnit3TestAdapter 6.3.0 discover and run as they did NUnit 4.6.1; xUnit.net v3 (`xunit.v3.mtp-off`
  4.0.1) in place of the deprecated `xunit` 2.9.3 in ControlPanel.Tests and ImportTool.Tests; Node.js 24 for the
  `harnesses` job, whose four harnesses give the same results as on Node.js 22; and QuickJS 2026-06-04
  (What changes on upgrade, item 14). Left, each for its reason: `global.json`'s SDK floor of 10.0.303, since
  `rollForward: latestFeature` builds with the newest .NET 10 SDK installed anyway and 10.0.401 would refuse a
  machine with 10.0.400 and Alpine's `dotnet10-sdk` 10.0.303; every Python version that Semgrep 1.178.0 (the newest
  release), OpenTelemetry 1.37.0, pydantic 2.13.5 or Playwright holds where it is - click 8.5, jsonschema 4.26, mcp
  1.30 and 2, OpenTelemetry 1.45, PyJWT 2.15, pydantic-core 2.49, boltons 26, importlib-metadata 9, peewee 4,
  protobuf 7, wcmatch 11, wrapt 2 and pyee 14 - so PyJWT 2.13.0's advisories stay dismissed in the VEX document;
  sse-starlette 3.5.0, googleapis-common-protos 1.75.5 and coverlet.collector 10.1.0, younger than the three days a
  release waits; the lab's MariaDB 11.8, the CI's PostgreSQL 18 and MariaDB 13, the packaging compose file's
  PostgreSQL 18 and GitLab's Semgrep analyzer 7, each a decision and a run of its own; and the package builds'
  distribution releases. `renovate.json` now moves a distribution's image and the lab's images by digest only,
  keeps the CI's databases, the compose file's PostgreSQL and GitLab's analyzers on their major version, no longer
  proposes `global.json`'s SDK, brings an OpenSSL patch release on any day in a merge request of its own, and asks
  Fedora's registry, which lost a lookup on each of three days, one request at a time.

- **The semgrep job counts a file Semgrep could not parse, or parsed leaving more than ten of its lines unread, as a
  hole in the scan**, as it did a rule that timed out: Semgrep reports both only as a warning, and the job had taken
  `build/ci/semgrep.sh` (not parsed at all), `container-image-sign.sh` (97 of 138 lines unread, every command that
  signs and attests the image) and `scorecard.sh` (117 of 243) as scanned; the three are now written so Semgrep's
  bash parser reads every line.

- **The regression tests that read a mailbox by the order its messages arrived in no longer depend on which delivery
  thread wins.** Each message is in the mailbox before the next is sent (FullTextSearch, which failed 6.3.5's gate
  once with "1 3" for "1 2"; RestApiRecoverDeleted, Discovery, ObjectStorage, and RestApiSelfService's subscriptions
  and keywords tests), and the quota warning, which is queued before the message that crosses the threshold is
  stored, is found by being the one message not sent rather than as the last one. About 290 other tests that deliver
  several messages were read and left as they are: they count, compare sets, find by content, or already wait.
- **`build/check-changelog.py` holds a release section's "Full notes" line to the section's end.** The line that sends
  the reader to the release page must be its section's last paragraph, once, linking that section's own release;
  release-publish ends the GitLab release's notes with the section's text, and a fold of parallel work on 1 October
  2026 had left 6.3.6's in the middle of it. Four self-test cases: the line mid-section, twice, linking another
  release, and not in its form.
- **`build/check-release-chain.py` holds the AppImages' two sources jobs to `.no-github`.**
  `appimage-runtime-sources` and `appimage-bundled-sources` fetch sources whose recipes name GitHub for five Alpine
  libraries; the checker now fails when either stops running `.no-github`'s `before_script` - taken out of its
  `extends`, or replaced by a `before_script` of its own or of a nearer template - and its self-test breaks it six
  ways.
- **A build step can be run with the network refused, on a runner that cannot `unshare`.**
  `build/ci/packages/offline.c` installs a seccomp filter under `no_new_privs` that makes `socket(2)` fail for every
  address family but `AF_UNIX` and `AF_NETLINK`, and `io_uring_setup(2)` fail, then runs the command; every child
  inherits it. The source packages' builds run under it, since linix's unprivileged containers are refused
  `unshare(2)` by Docker's default seccomp profile. It refuses making a socket, not using one: a socket open before
  it starts stays usable, and the jobs' shells hold none.
- **The version stamps and the exclusion list reach the source packages.** `build/check-linux-version-stamp.sh` holds
  the Debian changelog's one entry (`<version>+ds-<revision>`) and the spec's `Version:` and one `%changelog` entry
  to `Version.h`, so stamping a release is now fourteen edits; `build/check-source-tarball.py` holds
  `debian/copyright`'s `Files-Excluded` to the exclusion list plus test data the Debian build does not read, and the
  new build scripts to the list; `build/add-license-headers.py` treats only maintainer-script hooks and `rules` in a
  `debian` or `rpm` directory as scripts, since a changelog or copyright file cannot begin with a comment.
- **`legacy-blowfish-tests` and `check-blowfish-constants.py`.** LegacyBlowfishTester - ClassTester on Windows, the
  new `legacy-blowfish-tests` on Linux, which `linux-build.sh` and the source packages' builds run with the other
  `*-tests` - holds the cipher to Eric Young's 58 published vectors and the scheme to 18 values 6.3.4 wrote and 11 it
  read; `build/check-blowfish-constants.py` recomputes pi's first 8,336 hexadecimal digits and compares them with the
  cipher's 1,042 table words, with a `--self-test` in the `checkers` job; `API/Basics.cs` holds the COM API to the
  same values. The source packages' builds also run `singleton-tests`.
- **The documents say what 6.3.5's release did.** RELEASE.md, SECURITY.md, CONTRIBUTING.md,
  [ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  [UpdateFeed.md](hmailserver/docs/UpdateFeed.md), [WindowsOnArm.md](hmailserver/docs/WindowsOnArm.md) and
  [EnvironmentConfiguration.md](hmailserver/docs/EnvironmentConfiguration.md) said the release chain had never run,
  dated 6.3.5 the 28th, or described the update-feed trigger. They now give 29 September, what 6.3.5's tag pipeline
  showed and measured, and, in UpdateFeed.md's new "From 6.3.4 on, by the live update", issue #10's test: a 6.3.4
  service running as LocalSystem installed 6.3.5 unattended, without a rollback image since 6.3.4 carries no Sigstore
  bundle; one running as `NT SERVICE\hMailServer` verified it and left it to an administrator (`HM7463`); and the
  Linux live update from 6.3.4, tried the same day (below). RELEASE.md step 3 compares the `hMailServer.exe` line
  `windows-installer` prints with the hash the version's CHANGELOG section gives, since a pipeline release's
  verification section names the installer and not the executable in it.
- **The installer smoke test holds Windows Update off for its run.** Its VM resumes from a snapshot of 25 September
  2026 at every run, and on 29 September Windows Update installed an update and restarted it in the middle of a test.
  `build/ci/installer-smoke-run.sh` now stops and disables the Windows Update and Update Orchestrator services over
  SSH before it copies anything; the snapshot is left alone, so the next revert brings them back, and a failure to
  stop them is reported and does not stop the run.
- **Both Windows package jobs count, and the ARM64 one builds.** `windows-packages-x64` and `windows-packages-arm64`
  may no longer fail without failing the pipeline, and a tag's release waits for both. The ARM64 job's own script ran
  whole on the Windows runner by hand, as LocalSystem, on 29 September 2026: the ARM64 OpenSSL, Boost and libpq built
  into the runner's cache (17, 2 and under 1 minutes the first time), then the server, the tools and the installer in
  under six minutes, and every native file the installer packs checked to be an ARM64 image, but Microsoft's ARM64EC
  `vcruntime140_1.dll`, which an ARM64 process never loads. The runner's `PATH` is its service's own, so libpq's
  build tools are fetched by the job: `libraries/get-libpq-tools.ps1` takes Python 3.14.7 from nuget.org, Meson
  1.11.1 and Ninja 1.13.2 from PyPI and winflexbison 2.5.24 from SourceForge, each held to a pinned SHA-256, into the
  cache, installing nothing. `windows-packages.ps1` named a variable of its Boost step `$tag`, which PowerShell reads
  as its `-Tag` parameter, so every ARM64 build's `build-info.txt` named the tag `vc145`; fixed.
- **The Windows runner refuses GitHub.** bench150's hosts file sends `github.com`, `api.github.com`,
  `codeload.github.com`, `objects.githubusercontent.com`, `raw.githubusercontent.com` and `ghcr.io` to `0.0.0.0` and
  `::`, so that anything a job runs that reaches for GitHub fails by name; both installers built afterwards
  (ContinuousIntegration.md, "bench150").
- **The documents say what the first Linux live update showed.** On 29 September 2026 a 6.3.4 server from the
  `.deb` on Ubuntu 26.04 installed 6.3.5 unattended, and one from the `.rpm` on Fedora 44 by hand over REST, both
  without a rollback image. [UpdateFeed.md](hmailserver/docs/UpdateFeed.md), PackagesDeb.md, PackagesRpm.md, the
  packaging README and [Roadmap3.md](Roadmap3.md) say so where they said it was untried.
- **The documents say what this release's pipelines showed.** PackagesDeb.md and PackagesRpm.md said no install job
  had run in a pipeline; the nineteen first ran, and passed, in `batch41`'s 2887524317 on 28 September 2026, and
  every build and install job passed again in 6.3.5's tag pipeline and in `batch42`'s 2896082335 and 2897966924,
  which built 6.3.6 and hold each package to its licence files - each job's times are there now. PackagesAppImage.md
  said no pipeline had run the AppImage jobs, which first passed in 6.3.5's tag pipeline and, with both sources
  tarballs, in both of `batch42`'s; SourcePackages.md, that its seven targets were still to run, of which five have
  passed and openSUSE Leap 16.0 and Tumbleweed have not; ContainerImage.md, that `linux-image-arm64` had not run;
  ContinuousIntegration.md, that the AArch64 packages' SBOM jobs and the three scans had not; and Nix.md, that the
  `nix` job had not, which passed in 2896082335, as Alpine.md now says its three jobs did. The three package
  documents also said linix's qemu-aarch64 registration had not been seen to come back after a reboot; it came back
  at linix's reboot of 28 September, and the arm64 jobs ran under it. [Roadmap3.md](Roadmap3.md) says the same in
  fourteen rows - the chart's package, the `.sigstore.json` copies and the provenance among them, all three in
  2897966924's rehearsal of the release - and two are done: the package install and test jobs pass in a pipeline, and
  the texts that said what was no longer true now say what the tree and GitLab show. RELEASE.md's security release,
  step 10, said a server from 6.2.28 to 6.3.3 needs one `UpdateFeedUrl` line; it is upgraded by hand, once, as these
  notes say.
- **`backup-tests` and `trust-anchor-tests`**, two more tester programs `linux-build.sh` runs, and the source
  packages' builds too (`dh_auto_test` and `%check`, [SourcePackages.md](hmailserver/docs/SourcePackages.md)); both
  run in the class tests on Windows as well.
- **What the static analysis found in the core's types, its platform layer and its dead code is fixed, not
  dismissed (the SAST triage of 6.3.6).** None of it changes what the server does. The constructors of
  `Singleton<T>` and `BusinessObject<T>` are private and `T` is their friend, so a class that named another type as
  its `T` - a singleton that would be handed that type's `Instance()` - no longer compiles; `singleton-tests` holds
  both, and does not compile against the old headers. `TCPConnection`, `IMAPCommand`, `IMacroExpander`,
  `TCPConnectionFactory`, `NotificationClient` and `IPrerequisite` have virtual destructors: every object of theirs
  was owned by a `shared_ptr` made from its own class, which destroyed it whole, and one deleted through the base now
  would be too. `MySQLInterface` and `GssapiAcceptor`, and four classes of the same shape that the analysis did not
  flag - the database layer's connections and recordsets (`DALConnection`, `DALRecordset`), `ByteBuffer` and
  `FolderManipulationLock` - release a handle, a buffer or a lock in their destructors, and cannot be copied; nothing
  copied one. The unused `StringTreeParser` is deleted: nothing included it, its `Parse` never returned a node, and
  an unmatched quote gave `Mid` a negative length. A redundant test in the MIME parameter parser goes, and so do five
  C-style casts - two in `Unicode.cpp`, three in the POSIX build's `BSTR` functions (`hm_platform.h`) - and the code
  compiled from them is the same as before; the `ETRN` reply, whose two `else`s were indented as neither of their
  `if`s was, and four more places are indented as their braces are; and the `timeval`, `tm` and `timespec` of the
  POSIX `GetLocalTime`, `GetSystemTime` and `GetTickCount64`, and two more `struct tm` locals whose fill was not
  checked (`DomainSamples.cpp`, `ZipWriter.cpp`), start zeroed. 71 findings leave
  `build/ci/static-analysis-baseline.txt`, and the job, run over the tree, finds none that is new. A sweep of the rest
  of the tree for the same shape - a class whose destructor frees, closes, ends or gives back what it holds and that
  could still be copied - found sixteen more, and they cannot be copied now either: the external fetcher's manager,
  IMAP `APPEND`'s handler, the web services listener, the rooms' drain task, the event-script site, the live update's
  drain guard and the Updater's shutdown block, the indexer's second look, a test's key-file guard and seven handle,
  SID and security-descriptor holders of the elevated update stage and the service account's grants; nothing copied
  one. Twenty-two files that no build compiled are deleted: the eleven `CMakeLists.txt` listed as in no build
  (`LoggerFile`, `ServerThreads`, `AccountCache`, `OutboundPortConnection`, `Mutex`, `ISMTPCommand` and
  `SMTPCommandHelp` with their headers, and the empty `Collection.cpp`, `Cache.cpp`, `CacheReaderWithDbFallback.cpp`
  and `TestConnectionResult.cpp`), `IThread.h` and the DKIM COM class's three files, which no project named; the
  list goes from `CMakeLists.txt`, `build/linux-tu-census.sh` and `build/check-source-tarball.py`. The Debug build
  compiles again (`AutoBanFirewall.cpp` used `DEBUG_NEW` without defining it).
- **`connection-string-tests`, `language-tests`, `live-log-tests` and `profiles-tests`**, four more, run the same
  ways: the PostgreSQL connection string read back by libpq and the bytes MySQL's values are handed over in, with its
  database's name quoted, COM's languages across a `ProgramFolder` change, the live log switched on and off beside
  four logging threads - the harness a ThreadSanitizer build runs, whose header says how - and a profile verb asked
  for on the thread that holds the profiles' lock. All but `live-log-tests` run in the class tests on Windows too,
  beside the SQL Server connection string read back by ADO.
- **`DBSetup.Tests`**, run by the `windows-dotnet` job beside the Import Tool's: how `DBSetup /unattended` reads its
  `HM_DB_` variables - the quotes, backslashes and semicolons it takes and passes on as given, and the values
  `hMailServer.ini` or SQL Server's connection string cannot carry, refused before anything is changed.

- **`file-utilities-tests`, and a third mode of `SimulateSpoolWriteFailure`.** The tester program holds
  `File::Close`, `FileUtilities::WriteToFile`, `TraceHeaderWriter`, the message store's `WriteAtomically` and the
  spool a received message is written to (`TransparentTransmissionBuffer`, which must count the message as not
  written, and report it once) to a file that cannot take its last bytes: `/dev/full`, a quota of the program's own
  (`RLIMIT_FSIZE`) and, where it may mount one, a 64 KB tmpfs filled to its last byte, in a mount namespace of its
  own; a container that may not mount says the last case did not run. `FileUtilitiesTester`, which the class tests
  run on Windows, closes a named pipe whose reader has gone, which refuses a write as a full disk does. The
  regression suite's switch `[Settings] SimulateSpoolWriteFailure=3` lets every write of a received message land and
  has its close fail, for SMTP, the two fetchers and APPEND, off unless set, like modes 1 and 2. Static analysis: the
  report's `cert-err33-c` at `File.cpp` and the committer's empty catch are fixed, and their lines leave the
  baseline.

- **No CI script reads a long or still-running output through `head` or `grep -q` under pipefail.** A reader that
  stops at what it wants closes the pipe, and a command still writing then dies of SIGPIPE, which `set -o pipefail`
  makes the pipeline's status, 141 (linux-sanitize failed so in pipeline 2887524317). Measured in WSL, 20 runs each:
  a command that writes a line at a time piped into `head -n 2`, as `syft version` is in container-image-sbom and
  linux-sbom, failed 20 times; the build scripts' failure summaries, `grep ... | sort -u | head -n 80` over a long
  list, failed 20 times, and under `set -e` ended the AppImage and Nix scripts before their own message. Those, and
  `ldconfig -p | grep -q` in `build/lab/db-suite.sh`, `rpm -qp ... | grep -q` in `srcpkg-rpm.sh`, a shell `printf`
  of a variable into `grep -q` or `awk` in `container-image-start-test.sh`, `appimage-build.sh`, the packaging's
  `build-appimage.sh`, `test-install-kind.sh` and `test-hmailserver-update.sh`, and `helm template ... | grep -q` in
  the chart-and-compose job, which could pass a chart that wrote the Secret it is there to catch, now read a file
  themselves (`grep -m`), read their input to the end (`sed -n '1,Np'`) or search a file or a here-string. The
  scripts run under `sh` without pipefail, and pipes whose command writes a line or two at once, are unchanged.

- **The performance tools run as their pages say**, for the measurements after this release
  ([Performance.md](hmailserver/docs/Performance.md), [PublishedNumbers.md](hmailserver/docs/PublishedNumbers.md)).
  `webmail-timing.py --mode browser` looked for *Search older messages* inside the message list, where 6.3.5 no
  longer draws it, so its search stopped at the first answer and reported no continuation; and in an inbox shown in
  tabs, the default, its select-all and *Mark as read* changed the Primary tab's messages alone. It now presses the
  button under the list, and runs with the inbox in one list, putting the account's choice back afterwards - by a
  new sign-in when its own session has ended while the browser ran, and saying so when it cannot.
  `build/readme-shots/bench.ps1` stopped at its first line under Windows PowerShell 5.1, which the pages' `powershell
  -File` starts. `seed-folder.py` and `webmail-timing.py` take the account's password from `HMPERF_PASSWORD`, as the
  load generator takes its own from the environment. The recipes now take a pinned Python outside the bench's PATH,
  where a `python.exe` would change which tests the next gate runs; turn the logging off, which a gate leaves on; run
  the browser's select-all once, last, with the index on, since it leaves the folder read; wait five minutes after
  each restart; check the index has caught up by a search, not by its count, and leave it a margin after the search
  first says so; measure SMTP out with fsync off, and the Windows bench with fsync off at all; run a warm-up and
  three counted runs on Windows, each with `--server`; give the commands that put back what `prepare-hmailserver
  --cleanup` leaves; and give the seed's bytes for the page's own account, 1,322,787,827. `lab.sh numbers` records
  the served server's logging beside its fsync setting. The seeding, the timing and the load generator were run with
  the pages' command lines - the load generator's with the Linux process's name and data folder - at a pilot's size,
  against a server built from this tree on linix with PostgreSQL: 5,000 messages seeded at 50 a second; each search
  three requests with the index off and one with it on; the browser's search followed twice where it had stopped, and
  its *Mark as read* changed all 5,000 messages where it had changed the Primary tab's 2,999; and
  `prepare-hmailserver`, `smtp-in`, `smtp-out` and `imap-sessions` with the `/proc` sampler, fsync on and off.
  `bench.ps1 -Up` and `-Down` ran under Windows PowerShell 5.1 and PowerShell 7 against a stand-in for the COM
  object, and the pinned Python and Playwright installed as the page says under 5.1. The lab's comparison, imaptest
  and anything on the Windows bench were not run.
- **Static analysis: the server-logic findings of the 6.3.6 triage.** Besides the fixes above (an account's copy,
  an IP range's priority, the seven catch-alls): a message's copy now copies its base, which holds its id, as an
  account's does; `ChangeNotification`, `WorkQueueTaskInfo`, the work queue's running and pending tasks, the
  firewall's ban, `OidcSignIn::Mapping`, the list manager's taken command and the protected messages' lapsed row give
  every scalar member a first value; `DiagnosticResult`'s empty constructor sets its result to false, which a
  `hMailServer.DiagnosticResult` made directly over COM had left unset; and a script object is made with its type,
  which the compiler now requires. None of these was found read unset in the server's own use. The domain-sample
  prune's `struct tm`, whose fill it did not check, now starts zeroed, so that a day that cannot be converted prunes
  nothing rather than read what the memory held. `business-object-tests`, a new tester program that `linux-build.sh`
  and the source packages' builds (`dh_auto_test`, `%check`) run, makes an account's and a message's copies and
  four of these types over memory filled with a pattern, holds every member to what it should be, and holds the IP
  range's priority to what the update takes; with the fixes undone, the account's, the four types' and the
  priority's checks each fail, and the message's fails when a member is dropped from its copy. The 30 lines of
  `static-analysis-baseline.txt` whose findings these fix are gone.

- **Twenty static-analysis findings in the TLS and error-reporting code, fixed.** Beside the two Operations entries
  above (the TLS options mask; six failures said), the listeners' and the TLS initialiser's seven catches of
  `boost::system::system_error` take it by reference rather than copy it; `CipherInfo` throws its `std::logic_error`
  by value where it threw a pointer that no `std::exception` handler would catch and that leaked (no caller reads an
  empty `CipherInfo`, so it was never thrown); the DNSSEC resolver's socket timeouts and the journal socket's address
  are passed with C++ casts; MailboxKeys' reseal record starts with its account id and flag set; and a device's
  sign-in count saturates rather than overflows. Their lines are gone from `build/ci/static-analysis-baseline.txt`.

- **ControlPanel.Tests hold Live logs and the Server status page.** Live logs' reading of the server's log and its
  following of the end moved out of the page into `Services/LiveLogTail.cs`, unchanged, so that tests hold them:
  whole lines only, a tail begun in the middle of a line, a rotated log, following only while the end is in view,
  and the line being read held in place as the oldest are dropped (13 tests; the real page, run in-process before
  and after, follows, holds and trims the same). The Server status page's captions and values are held to be text
  that can be selected and copied (4 tests). `build/check-localisation.py` has a `--self-test`, which the
  `checkers` job runs: lines and translations made to pass and to fail its rules, among them each shape it missed
  before 6.3.6. ControlPanel.Tests: 974 tests before, 1,017 after.

- **Forty-one static-analysis findings - members left unset, dead code and tests already implied - fixed (the
  SAST triage of 6.3.6).** None of it changes what the server does. Twenty-seven structures that are filled field
  by field - the archive index's and the ZIP writer's entries, the SPF macro reader's result, the S/MIME key rows
  and twenty-three more - and the logger's copies of `LogLevel` and `MaxLogLineLen` and the SMTP session's message
  start time, which their constructors left unset and which are written before they are read, start at zero,
  `false`, a null pointer or an enumeration's first value, so a field that a later change forgets to fill holds a
  known value rather than whatever the memory held; on Linux, the `timespec` that the crash record and an
  OpenTelemetry span read with an unchecked `clock_gettime` starts zeroed. A MIME body's part iterator starts at the
  end of its empty part list, where `Clear()` puts it, rather than unset, and `MimeHeader` and `MimeBody` call their
  own `Clear()` by name in their destructors, the call a destructor makes anyway. Dead code goes: `CachedObject`'s
  default constructor, which nothing called and which left its id, creation time and size unset; the three
  `*_MEDIATYPE` macros, which nothing used and the first of which could not have compiled; and two tests already
  implied where they stand - a public-folder test inside the public-folder branch of IMAP `CREATE`, and `== -2`
  beside `< 0` in the update verifier's certificate-expiry check. Their 41 lines are gone from
  `build/ci/static-analysis-baseline.txt`.
- **`build/check-db-scripts.ps1` refuses a SQL Server upgrade statement marked `[IGNORE-ERRORS]` whose failure ends
  the transaction.** On SQL Server a failed `ALTER TABLE ... DROP CONSTRAINT` or `CREATE INDEX` rolls back the
  walker's transaction - DBUpdater's, which holds the whole upgrade, among them - whatever the marker and
  `XACT_ABORT` say, and the statements after it then commit one by one, the version row among them; a failed
  `DROP INDEX` does not (measured on SQL Server 2022). The 6107 step's script began with such a drop, of a
  constraint step 6072 had already removed, and no longer does; step 6072's own, which never failed, is the one
  exception the check knows by name.
- **`channels-check` runs when `.gitlab-ci.yml` changes.** The job that runs every release channel script's
  self-test and proves their verification with the pinned cosign ran only when a channel's own files changed;
  `.gitlab-ci.yml`, which pins that cosign (`HM_COSIGN_IMAGE`), and `.gitlab/ci/no-github.yml`, whose `.no-github`
  the job takes first, now count among them.

- **One Linux suite at a time on linix.** `linux-suite` and `alpine-suite` (`.gitlab/ci/alpine.yml`) share the
  resource group `linix-suite`, so that the two, each hours long, no longer run side by side on linix's runner,
  which takes more than one job at once, and two pipelines' suites no longer share it either.

- **The AppImage runtime's sources outlast a short outage of Alpine's hosts.** `appimage-runtime-sources.sh` tried
  a download three times, and only after a timeout or a 408, 429 or 5xx answer; a connection refused or dropped
  failed the job at once, and the aports fetch and `apk add` were tried once. A failed, timed-out or dropped
  download is now tried again with a growing wait for up to ten minutes, as `appimage-bundled-sources.sh` already
  did (a 404 is not, and still sends it to `edge/`'s mirror at once), and the aports fetch and `apk add` three
  times over a minute and a half; every file is still held to the SHA-512 its `APKBUILD` gives and every aports
  directory to its commit. `appimage-bundled-sources.sh`'s `apt-get update` retries as its install did. In the
  job's `alpine:3.24` against a loopback server, a download dropped twice and then served arrives, which the old
  function gave up on at the first drop, and a 404 is asked for once.

- **`check-release-chain.py` refuses a third platform's scan or start.** Its rule that the container image is built
  for linux/amd64 and linux/arm64 alone skipped every job named `container-image-start*` and never read the
  `container_scanning_*` jobs, so a `container-image-start-riscv64` or a `container_scanning_riscv64` passed it; both
  are refused now, and the push script's check that each image is for the architecture its job is named for is held
  to the file too. Three new `--self-test` cases, each passed by the checker as it was.

- **`repo-hygiene`'s Pinned-Dependencies reads the included CI files.** It read the images of `.gitlab-ci.yml` alone,
  27 of them, and the installs of `build/*.sh` and `build/ci/*.sh`; most of the jobs are in `.gitlab/ci/*.yml`. It
  now reads the images and installs of every file `.gitlab-ci.yml` includes and of every shell script under
  `build/`: 91 images in 23 files, every one pinned by digest or, for the CI image, by its Dockerfile's hash, and no
  piped download. A `pip install --no-index` of a local wheel, which no index can change, is not taken for an
  unpinned install (`cli-packages` installs its own wheel so). Its `--self-test`, which the `checkers` job runs,
  has four new cases - an image, a service and an unhashed `pip install` in an included file, each caught, which the
  check as it was passed, and a `--no-index` install, passed.

- **Both halves of `build/check-cli.py` run in CI.** The PowerShell module's half - its commands against the recorded
  server, 502 checks - never ran in a pipeline: the `checkers` image has no pwsh, so the run there skipped it.
  `cli-powershell` (`.gitlab/ci/cli-packages.yml`), in the .NET SDK image whose pwsh `psgallery-publish` uses, now runs
  `build/check-cli.py --powershell` after the manifest and analyzer checks, and runs when the checker changes too;
  `checkers` runs `hmctl`'s half (`--python`, 1,124 checks) by name. Asked for alone, the PowerShell half that cannot
  run now fails rather than passing as skipped. In the job's image on linix the module's 502 checks passed; a route
  planted wrong in `HmailServer.psm1` failed three of them.

- **The arm64 jobs ask whether linix still runs aarch64 programs, and say so when it does not.** On 29 September
  a privileged container running systemd unregistered every binfmt_misc handler on linix as it stopped, and every
  arm64 job after it failed with "exec format error", most before their scripts started. The arm64 and aarch64
  builds of the `.deb` and `.rpm` packages, `appimage-aarch64-compile` and `linux-image.sh` for arm64 now run
  `build/ci/arm64-emulation.sh` first: it writes a 132-byte aarch64 program that exits with 42, runs it, and refuses
  the job by name, with what to do on linix, when anything else comes back. The install and AppImage jobs that run
  in arm64 containers cannot ask for themselves - their shells are aarch64 programs - so the build before each asks;
  `appimage-runtime-aarch64`, which follows nothing, still meets the error itself. Proven on linix in the images of
  the jobs that run it (busybox, dash and bash; kaniko without `/tmp`), and refused in a user namespace with an empty
  binfmt_misc of its own, the host's 31 handlers untouched.

- **A tag builds the x64 installer once.** A `v*` tag's pipeline built it twice on bench150: in `windows-installer`,
  whose file the release carries and `release-authenticode` signs, and in `windows-packages-x64`, whose bytes nothing
  took, at the cost of another build under the gate lock before the ARM64 build, which `release` waits for. The
  maintainers decided on 1 October 2026 that a tag builds it once, in `windows-installer`: `windows-packages-x64`
  refuses a tag, and `windows-packages-arm64` follows `windows-installer` there instead of it (an optional need, which
  a branch still meets). `build/check-release-chain.py` holds both, each with a negative control; the batch and
  master pipelines, and the release rehearsal, still build and collect `windows-packages-x64`'s installer
  (PackagesWindows.md, "One x64 installer for a tag").

- **A cancelled job's compilers no longer break the next checkout on bench150.** In pipeline 2897966924 a cancelled
  build left `cl.exe` and `mspdbsrv.exe` running in the runner's checkout, holding a precompiled header open, and the
  next job's checkout failed until they were killed by hand. Every job on bench150 now runs
  `.bench150-leftovers`' `pre_get_sources_script` before anything is fetched: it stops each process of the runner's
  account named as a compiler, MSBuild, its tracker, the .NET build server or test host or Inno Setup's compiler whose
  working directory - read from the process itself - is the job's own directory or under it, or whose command line
  names it, and logs which and why; never another account's process, the runner, another job's directory or
  `hMailServer.exe`. `build/check-release-chain.py` holds every bench150 job to it. On the laptop, five planted
  leftovers (64- and 32-bit, by directory and by command line, one holding a file open) were stopped and four that
  must stay were left; without the step all five stayed and the open file could not be deleted.

- **`build-openssl.ps1` finds a version Debian's pool has dropped, and cleans up its download.** It takes OpenSSL's
  release archive from Debian's archive, which ships it unchanged, and stopped with "find it on snapshot.debian.org"
  once a version had left the pool; and the download it made stayed in the library folder. It now looks the version up
  through snapshot.debian.org's API and downloads the `.orig.tar.gz` by the SHA-1 snapshot gives
  (`Get-DebianOrigArchive` in `libraries/build-common.ps1`, which tries a failed download again three times and never
  follows a redirect), holds it to the same pinned SHA-256 as before - a file that does not match is deleted and
  refused - and deletes its download once the tree is extracted; a copy given with `-Archive` is still the caller's.
  Under PowerShell 7 and 5.1: 4.0.3 from the pool (x64 and ARM64) and 4.0.1, gone from the pool, from snapshot, each
  extracted with nothing left behind; a wrong digest and a version in neither place refused. The script as it was
  failed all five.

- **The merge request checks, tightened.** `semgrep-sast-gate` printed up to twenty of the tree's unaccepted
  Critical and High findings outside a merge request's changed files in its public log; it counts them now and
  describes none, the release's gate holding them as before. A merge request that changes `.gitlab-ci.yml`, whose
  variables for every job reach the analyzer, or `.gitlab/ci/mr-checks.yml`, which runs the gate, answers for every
  finding in the tree, as one that changes the scan's own file did. `mr-dependency-check` rated an advisory by its
  GitHub label or CVSS 3.x vector alone, so one carrying only a CVSS 4.0 vector held nothing; it now scores CVSS 4.0
  vectors by FIRST's method - the same as Red Hat's `cvss` 3.6 on all 419,904 base vectors with each Exploit
  Maturity and on 300,000 random ones with environmental metrics - and reads a malformed one, such as `E:AP` or an
  empty value, as no vector, as it does a malformed 3.x one, where a first cut stopped the check with an error. And
  an advisory it could not decide - a version, or a range's bound, that it cannot order as NuGet or PEP 440 does -
  passed; Critical or High, it now holds the merge request as one that may affect the version. Each with
  `--self-test` cases the checks as they were failed.

- **The container image jobs, hardened.** `container-image-deb` verified each `.deb` and SBOM it took from the
  published release as signed by the release's pipeline, but a signature binds bytes, not a name: another file the
  pipeline signed, served under a `.deb`'s name, verified too. It now also takes the release's signed
  `hmailserver-linux-<version>-SHA256SUMS.txt` and holds each `.deb` to the SHA-256 it gives that name, and each SBOM
  to naming its `.deb`'s SHA-256. Against a mirror of 6.3.5's published files, the release as it is passed, and its
  `.rpm` under the `.deb`'s name and the `.rpm`'s SBOM under the `.deb`'s were refused - both of which the job as it was
  took. `container-image-sign` waits for both start tests, so an image that does not start is never signed. And a
  plain `docker build` of the Dockerfile no longer labels the image with Ubuntu's `24.04` as its version: it says
  `local`, or the version given with `--build-arg HM_IMAGE_VERSION`, which `linux-image.sh` now passes as well as its
  label. `build/check-release-chain.py` holds the three, each with a negative control.

- **The build provenance keeps its one name, and `repo-hygiene` counts it.**
  `hmailserver-<version>.intoto.sigstore.json` is a Sigstore bundle holding the provenance's DSSE envelope, and is
  named as one; OpenSSF Scorecard counts only `*.intoto.jsonl` as provenance, the SLSA generator's name for JSON
  Lines of bare DSSE envelopes, so it counts this file as a signature. Decided: no copy under that name, which would
  misdescribe the file's format to raise a score (SECURITY.md, "Build provenance"). `repo-hygiene`'s Signed-Releases
  check, which read the same name alone, counts it as provenance now, and says why Scorecard does not.

- **Each PPA upload stands alone.** `ppa-upload` sent the release's orig tarball with its first Ubuntu release's
  upload alone, and the next upload only named it: had Launchpad taken the second first, or refused the first, the
  second would have been refused too. While the PPA holds no orig of the version, every release's upload now carries
  it, the same bytes each time, which Launchpad takes again as it is ("Troubleshoot package upload errors": it refuses
  a file it holds only when the bytes differ); once the PPA holds one, none does (`series_flags`, with a
  `--self-test` case for each).
- **`static-analysis` and `semgrep` fail the pipeline on a new finding.** The two source scans, cppcheck and
  clang-tidy and Semgrep's open rules, held their findings to committed baselines but were `allow_failure`, so a new
  finding was a warning in a branch's pipeline and only the release's gate read it. They are not `allow_failure`
  now: a finding new to its baseline, or a scan with a hole, fails a `batch*` branch's pipeline and a tag's, and
  `build/check-release-chain.py` fails while either is `allow_failure` again (a `--self-test` case for each, which
  the check as it was passed). Each passed first over `batch42` with the change folded in, on linix in its own
  image, once what it found new since its last pipeline was read: a cast in the vendored QuickJS header that the
  QuickJS update brought in, upstream's code, which cppcheck now leaves out with the rest of the engine as
  clang-tidy did; a `subprocess.run` with an argument list in `build/check-deck-script.py`, marked where it is; and
  the MD5 of the greylisting key the database-upgrade gate recomputes, in the baseline with its reason. And
  cppcheck is given `_T` as the POSIX build defines it, since it does not read the header the build forces into
  every file from the compile database: two literals joined, `_T("a") _T("b")`, made it give up on a test program's
  file, a hole in the scan that fails the job. A cppcheck finding planted in the report fails the job. `static-analysis` has two hours where it had 90 minutes, since a
  run cut off now fails the pipeline: it took 72 minutes on a busy linix. GitLab's `semgrep-sast` over the same
  tree had 22 High findings that no exception accepted; each was read against the code - a buffer sized and given
  in characters, copies bounded by their arrays, string literals, the system's environment block, and two programs
  started by full path with arguments that cannot be split - none is a defect, and each is accepted in
  `.gitlab/security-exceptions.yml` with its reason, for a year.

Full notes: [the 6.3.6 release](https://gitlab.com/hmailserver/hmailserver/-/releases/v6.3.6).

---

## [6.3.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5) - 2026-09-29

Everything that landed on `master` after the `v6.3.4` tag, from 27 September 2026 ([compare `v6.3.4` with
`v6.3.5`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.4...v6.3.5)). The schema moves from **6104
to 6105 in one step**, which adds a column to the quarantine's table, `hm_quarantine.quarantinehelo`. What changes
on upgrade without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has
five more of them than it had at 6.3.4.


This is the first release built to be made by the project's GitLab CI from its tag ([RELEASE.md](RELEASE.md), *The
release on GitLab* and *On GitLab, from 6.3.5*). The tag's pipeline builds the Windows installer on the project's
Windows runner after the whole Windows suite has passed there; once the owner has started the `release` job, it
Authenticode-signs the installer through Azure Artifact Signing with an RFC 3161 timestamp, signs every file of the
release with Sigstore as that pipeline -
`https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5`, issuer `https://gitlab.com` -
into a `<file>.cosign.bundle` beside it, and checks every bundle with the server's own `--verify-update` before
anything is published. That identity is the one a 6.3.4 server trusts (`SigstoreVerifier.cpp`, unchanged since
6.3.4, and the verifier that check runs), so the live update of a 6.3.4 server is built to install this release, on
Windows and on Linux. A server from 6.2.28 to 6.3.3 trusts only the GitHub workflow that signed the releases up to
6.3.3, and as it stands its live update refuses this one: install 6.3.4 or this release by hand, and keep
`UpdateAutoDownload` off until then, or on Windows give it the GitLab signer as
[UpdateFeed.md](hmailserver/docs/UpdateFeed.md) describes. What the release carries, and what the new package jobs
leave in CI only, is under Operations.

### What changes on upgrade without a switch

1. **The Control Panel's sidebar is in the order a new administrator needs.** Welcome and Dashboard come first,
   then a new group, **Set up this server**: the **Setup checklist**, which was *External setup* under Monitoring &
   troubleshooting, then Setting profiles and DNS records. Two groups are renamed - *TLS & certificates* is
   **Certificates & encryption** and *Access & abuse protection* is **Sign-in & access** - and Connections &
   protocols gains a **Webmail** page (Administration, below). The palette (Ctrl+K) still finds the checklist as
   External setup. A page's `nav-<key>` UI Automation id is unchanged; the `navgroup-` and `navrail-` ids of the
   new and the renamed groups follow their new headings, so a UI test that finds a group by its old id must be
   changed.
2. **The quarantine shows the HELO name, and groups.** The Control Panel's Quarantine page and the Control Deck's
   Quarantine view gain a HELO column and group the held messages by the sender's domain unless told otherwise.
   Messages held before the upgrade have no HELO name recorded and are grouped with those that have no HELO
   domain. The Deck now asks before a delete with a Yes or No of its own whose default is No, as the Control Panel
   already did; it used the browser's `confirm()`, whose default is OK. It offers whitelist and blacklist only to a
   credential holding `manage_settings`, and a held message's headers and preview only to one holding
   `view_mailbox`, as the server requires (below).
3. **The webmail's keyboard.** The message list is one Tab stop, where each row put its box, star and marker in
   the Tab order, and the list's keys reach what a row holds. Enter on a button in the mail view presses the
   button; it used to open the message under the list's cursor. A letter typed into the formatted signature is
   text, where `c` opened a new message. Escape closes every modal dialogue as its Cancel does and gives the focus
   back to where it was, the page under a dialogue cannot be reached until it closes, and menus close on Escape.
   The event and task editors close when the page changes. The compose window's *Plain* is **Remove formatting**,
   its tooltip's words. On a phone a row's importance marker is shown and not pressed; the message's More menu
   marks it.
4. **An invitation sent the way Outlook sends one is shown as an invitation.** `GET /api/v1/me/messages/{id}`
   answers `"invitation": true` for a `text/calendar` part whose `Content-Type` carries parameters and which has no
   `.ics` name, where it answered `false`, and the webmail shows the invitation's card with Accept, Maybe and
   Decline (the fix is under IMAP, POP3 and the webmail).
5. **What a script sees.** `GET /api/v1/quarantine` gives each message three more fields - `helo`,
   `sender_domain` and `helo_domain` - and four routes are new. Over COM, `Quarantine` gains `Bulk` (dispid 9)
   and `QuarantinedMessage` gains `HeloName`, `SenderDomain`, `HeloDomain`, `ReadHeaders` and `ReadPreview`
   (dispids 11 to 15), added to the existing interfaces; no existing member or dispid changes. A program built
   against 6.3.5's type library that calls one of them needs a 6.3.5 server.

### Anti-spam, anti-virus and mail flow rules

- **Reviewing the quarantine** (schema 6105; [GitLab issue #1](https://gitlab.com/Progressiverobot/hmailserver/-/issues/1)):
  - **The HELO name** the client gave is stored with each held message. A message held during the SMTP
    conversation records the session's own; one held later, at delivery - by a rule, a block list, an account's
    own threshold, the sandbox or impersonation protection - takes it from the `Received` header this server wrote
    when it accepted the message, and only when that header names this server, so a `Received` line the sender
    wrote is never read. Empty for a client that gave none, for a message that did not arrive over SMTP and for
    every message held before the upgrade.
  - **Grouping**: by the sender's domain, by the HELO name's domain or not at all, each group folding, the group
    with the newest message first, and every column sorting the messages inside the groups. The domain is the
    organization's - the registrable domain by the Public Suffix List - which the server works out, so that every
    page groups a message the same way.
  - **Many at once**: a box on every message, every group and the header, and four actions on a message, a group
    or the checked messages - release; delete; whitelist, then release; blacklist, then delete. Whitelist and
    blacklist write the lists that already exist and no third one: the sender's address on the server's allow
    and block list (the default), or the organization's domain as `.example.com` on the domain white and black
    list, matched on the envelope sender, or on the HELO name when the page is grouped by HELO domain. Several
    messages from one sender or domain make one entry, and an entry already there is changed rather than
    doubled. The dialogue first shows what the server would write (a dry run); a domain that is itself a public
    suffix (`co.uk`, a HELO of `localhost`) is written only after a box is ticked, and without it the server
    refuses with 409 and changes nothing. The entries are written before any message is acted on, so a message
    whose entry could not be written - the null sender, no HELO recorded, a full list - is left in the
    quarantine with the reason.
  - **The headers and a plain-text preview** of a held message, read without releasing it: the headers up to
    65,536 characters, and the preview up to 16,384, the text part or the HTML part with its markup, style sheets
    and scripts taken out. Nothing is rendered as HTML.
  - **The Control Deck** is level with the Control Panel: the same groups, columns, boxes, icons and dialogues, one
    request for the checked messages, a delete dialogue of its own with No as the default, and Remove expired.
    Against a server from before this release, the Control Panel releases and deletes one message at a time, and
    its whitelist and blacklist say the server needs upgrading.
  - **REST**: `POST /api/v1/quarantine/bulk` (1 to 1,000 ids and one action, with `dry_run` and
    `confirm_public_suffix`), `POST /api/v1/quarantine/expire`, and `GET /api/v1/quarantine/{id}/headers` and
    `/preview`. The list, the headers and the preview need `view_quarantine`, and the headers and the preview
    `view_mailbox` beside it, since they are the mail itself; `bulk` and `expire` need `manage_quarantine`, and a
    `bulk` that allows or blocks needs `manage_settings` beside it, since it changes who is let in for everyone.
    Every one of them is refused to a credential confined to named domains, as the quarantine's other routes are.
    **COM**: `Quarantine.Bulk` takes and answers the same JSON, and each `QuarantinedMessage` has `HeloName`,
    `SenderDomain`, `HeloDomain`, `ReadHeaders()` and `ReadPreview()`; `Bulk`, `ReadHeaders()` and
    `ReadPreview()` are the server administrator's, as the rest of the quarantine is over COM
    ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md#reviewing-the-quarantine)).
- **A HELO that is an address has no domain.** A client that says `HELO 192.0.2.7`, bracketed or not, and a sender
  `someone@192.0.2.7`, have no organization's domain: `helo_domain` and `sender_domain` over REST, and `HeloDomain`
  and `SenderDomain` over COM, are empty, the message is grouped with those that have none, and whitelisting or
  blacklisting it by domain leaves it where it is and says why: "The HELO name is an address, which the domain list
  does not name." for the HELO, and "The sender's address has no domain to list." for the sender. Handed to the
  Public Suffix List, whose implicit rule takes the last label for a top-level domain, `192.0.2.7` would have been
  the domain `2.7`, and blacklisting it would have refused at `MAIL FROM` every client whose HELO is an address
  ending in `.2.7`.
- Fixed: a rule with a **Readable text** or **Sensitive content found** condition, which 6.3.4 added, read a
  message's HTML part in time that could rise with the square of its size. The part's entities were unwrapped by
  a replacement that moved the rest of the text once per occurrence, so 400,000 `&amp;` - two million characters -
  held the delivery for about a minute, and on a server with such a rule anybody could send one. Each entity is
  now unwrapped in one pass into a new string, with the same result (`ContentPolicy::StripHtml`). The quarantine's
  preview ends in the same function, and takes the style sheets, scripts and head out of an HTML part in one pass
  too, so a held message's preview costs time in proportion to its size.

### IMAP, POP3 and the webmail

- **The webmail, audited on a phone and a desktop, light and dark, by keyboard and through the accessibility
  tree**, with a seeded mailbox, and what it found fixed. On a phone: the search box has room to type and its
  options are a sheet the width of the screen; the tour skips a step whose target a phone does not draw and keeps
  its card on the screen; help covers the page instead of squeezing it; the settings tabs are one line that
  scrolls; and everything a finger presses is at least 44 pixels each way. By keyboard: the changes above, a focus
  ring on about twenty kinds of control that had lost it, and the earlier messages of a conversation and the
  Storage page's rows opened by keyboard as well as by mouse. For a screen reader: an unread row says Unread, a
  narrow list's tabs are named with their unread count and are a tab list, the row under the list's cursor is
  said, and the hidden page headings are translated. Contrast is raised where it fell short: the initials on the
  avatar colours, white on the dark theme's pale accent and danger colours (dark text now), the placeholders, and
  the light theme's warning colour (5.5:1, from 4.3:1); and the page declares its colour scheme, so that the
  browser draws its own parts of a control for the dark theme. Also: a long attachment name is cut with an
  ellipsis, and the week and agenda views name their month.
- **The webmail heard through a screen reader.** Every view was walked with NVDA 2026.2 in Chromium, in English
  and German, and what it said fixed. The message list is one control that reads each message as one line, its
  state first ("unread, starred, Grace Hopper, Re: The migration plan, Sep 26, has attachments, 3 of 51"), where
  Tab used to read all fifty rows and each row's controls; Home, End and the list's own keys work in it, Enter or
  `o` opens a message at its subject and Escape comes back to the same row, and Shift+F10 opens a message's menu
  from the keyboard. The star says whether it is pressed. What happens without the focus moving - a toast, once,
  not its countdown; a search's count; an attachment added; a new calendar month; a send that failed after the
  undo delay, which now also reopens the message and says why - is said in one place, and `z` presses a toast's
  Undo. Address suggestions, the command palette and mentions say the suggestion Enter will take; folders say
  their unread count in words and a subfolder its parent; the current folder, page, tab and view are said;
  dialogs and help open at their headings; the logo, the avatars' initials and a duplicate heading are hidden
  from a screen reader. 82 texts that were English on a translated page are in all 23 languages, and the server
  has a browser check the webmail's language file on each use instead of keeping it for a day, so new texts are
  not left in English after an upgrade. A dialog's name is said twice as the focus enters it, and a colour
  picker's name is not said; both are NVDA's reading of what Chromium exposes. Roadmap2 row 43 is done.
- Fixed: an invitation whose calendar part carries parameters in its `Content-Type` and no `.ics` name - the shape
  Outlook sends - was never marked as an invitation, so the webmail showed no card for it. The part's media type is
  now read without its parameters, as the server's iMIP code already read it. 6.3.4 had it too.

### Administration

- **Every database setting that should have an editor has one in the Control Panel.** 146 of the 314 settings the
  server keeps in its database had no editor in the Control Panel, and 137 of them needed one: the webmail's
  sign-in (passkeys, OpenID Connect, SAML, account recovery), its sessions and limits, OpenPGP, Web Push, the
  alerts, the SNMP agent, the audit trail, outbound webhooks, the spam traps, the complaint feedback loops, the
  reputation watch, country blocking, compromised-account detection, the object store, the managed lists and the
  migrations. They are now on the pages an administrator looks for them on - new tabs on Delivery of e-mail,
  Anti-spam, Auto-ban, Logging, Performance and Advanced - and a new **Webmail** page, under Connections &
  protocols, holds the webmail's own: Sign-in, Browser sessions, What people may do, Encryption, Notifications and
  Import. The 34 a COM property carries are edited over COM. The other 103, which the server reaches over its REST
  API alone, are read and saved over that API, which is off as shipped (`RestApiPort` 0): with it on, the Control
  Panel signs in to it once with the credentials it signed in with, asks for the administrator's one-time code once
  when there is a second factor, and sends only the keys that changed; with it off or out of reach, each such tab
  says so, and why. The domain dialog gains Country blocking, OpenPGP and Webhooks tabs, the account dialog an
  Identity tab (a person, a room or a piece of equipment, and the external ID SCIM matches on), an external account
  the options for a mailbox several people share, and a managed list's member how they receive the list's mail.
  `build/check-db-settings-coverage.py` now fails any database setting with no editor and no recorded reason for
  having none; the other nine have such a reason.
- **The Control Panel, audited page by page** in both themes at two window sizes, with each colour measured. The
  server status at the top right is a filled box with no outline - Connected is green on a soft green fill,
  6.0:1 in the light theme and 5.9:1 in the dark - and every status and notice is at 5.5:1 or better; High
  Contrast keeps a one-pixel outline. The light theme's field hints and captions are at 5.3:1, where they were
  3.3:1. Toasts are on the theme's neutral surface, with their level in the title and a glyph, and a failure - a
  page that could not load, a reinitialise that failed, a page partly saved - has a style of its own and stays up
  for eight seconds. A header or a cell too narrow for its text ends in an ellipsis and shows the whole of it as a
  tooltip. A list's Delete is one style, red words while it can be pressed and grey while it cannot; Incoming
  relays' and IP ranges' Properties and Delete need a selection, as every other list's do. The Domains page's
  alias and list boxes have captions. The **Setup checklist** numbers its items in the order a new installation
  meets them and marks the first one that needs action as the next step, and each item's button names the page it
  opens; the dashboard's attention card numbers and marks its items the same way
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md),
  [ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)).
- **The Control Deck's census** ([DeckParity.md](hmailserver/docs/DeckParity.md)) counts 498 of the 508
  properties the Control Panel writes as writable over REST and reachable from a Deck view; the ten it lists as
  missing are an account's directory fields and its external ID, and the server's country policy. The census had
  missed 16 COM members whose help string holds a `]`.

### Operations

- **Packages for Debian, Ubuntu, Fedora, RHEL and openSUSE, the AppImages and both Windows installers, built on
  GitLab CI** on the project's own runners, never for a merge request, and with no tool from GitHub:
  - **Debian and Ubuntu**: Debian 12 and 13 and Ubuntu 24.04 for amd64 and arm64, and Ubuntu 26.04 for arm64, as
    `hmailserver_<v>-1~<distro>_<arch>.deb` - Ubuntu 26.04's as `hmailserver_<v>_arm64.deb`; the amd64 package for
    Ubuntu 26.04 is the release's own, below ([PackagesDeb.md](hmailserver/docs/PackagesDeb.md)).
  - **RPM**: Fedora 44 and 43, RHEL 10 and 9, and openSUSE Leap 16.0 and Tumbleweed, for x86_64 and aarch64, as
    `hmailserver-<v>-1.<disttag>.<arch>.rpm`, with Boost linked in and every other `Requires` resolved from the
    release's own repositories; the executables are PIE with full RELRO, the stack protector, stack-clash
    protection and `_FORTIFY_SOURCE=2` ([PackagesRpm.md](hmailserver/docs/PackagesRpm.md)).
  - Each `.deb` and `.rpm` is built in its distribution release's own container with that release's compiler and
    libraries (the arm64 and aarch64 ones cross-compiled), then installed in a clean container of the same release
    and architecture, run and removed. They are built for every `v*` tag and `batch*` branch, and on `master` when
    started by hand.
  - **AppImages** for x86_64 and aarch64, built on Ubuntu 24.04, so for a host with glibc 2.39 or later and
    OpenSSL 3, with a runtime of the project's own (`platform/packaging/appimage/runtime.c`) in place of the
    AppImage project's, which is published only on GitHub, and without linuxdeploy; each is run again on
    Fedora 44. They are built for a `v*` tag, and elsewhere when started by hand
    ([PackagesAppImage.md](hmailserver/docs/PackagesAppImage.md)).
  - **Windows**: the x64 and the ARM64 installer, unsigned, in every pipeline of `master`, a `batch*` branch or a
    `v*` tag, after the Windows suite. The ARM64 job may fail without failing the pipeline, and in this release it
    does: its toolchain check looks for Visual Studio's ARM64 C++ runtime by a path that finds it in no
    installation, so it stops on the Windows runner, which has the ARM64 tools, ATL and SDK libraries, and prints a
    command that changes nothing. The check is corrected for the next release
    ([PackagesWindows.md](hmailserver/docs/PackagesWindows.md)).


- **What the release carries.** The release collects the x64 installer built by its own `windows-installer` job,
  the amd64 `.deb` and the x86-64 `.rpm` that `linux-build` makes (clang, in the CI's Ubuntu 26.04 image), the
  `hmailserver-linux-<v>-SHA256SUMS.txt` of those two, the server's SPDX and CycloneDX SBOMs and each compiled
  file's own `<file>.cdx.json`, every file with its Sigstore bundle (`build/ci/release-chain.py`). The packages
  above - every other `.deb` and `.rpm`, the AArch64 ones, the AppImages and the ARM64 installer - are CI job
  artifacts only, kept for 7 to 30 days: they are not signed, not in the release and not offered by the update
  feed. A server installed from one of the other x86-64 `.deb` or `.rpm` packages is offered the release's by its
  live update, whose dependencies its distribution may not meet; apt or dnf then refuses it, and the server stays
  as it was. One installed from an AArch64 package, an AppImage or the ARM64 installer is told that the release
  carries nothing for it to download.

### Build and tests

- **Reproducible builds on both platforms** ([ReproducibleBuilds.md](hmailserver/docs/ReproducibleBuilds.md)).
  `hMailServer.exe` builds to the same bytes from a checkout at any path: the Release configuration now trims the
  checkout's root and the libraries' root from every path the compiler writes (`/d1trimfile`), where it trimmed
  only the project's own directory, so that sixteen of the server's source files and 31 Boost headers left their
  absolute paths in the executable. 6.3.4 and the releases before it reproduce, if at all, only from a checkout and
  libraries at the paths they were built at. On Linux a new `linux-repro` job builds the server, the `.deb` and the `.rpm` a second time - from a copy of
  the tree at another path, without `.git` and with other file modes, in another build directory, under another
  umask and time zone - and fails unless all three are byte for byte `linux-build`'s; the release waits for it.
  For that, the packages now have the same modes whoever makes them - their directories 0755, the `.deb`'s
  maintainer scripts 0755 and its `conffiles` 0644 - and the `.rpm` records the commit's time as its build time
  and `reproducible` as its build host. Not claimed: the installer, the Updater and Minidump helpers, the .NET
  programs and the signatures. The release's runbook builds the executable once more from a second checkout
  (RELEASE.md, step 8).
- **Coverage on Linux, with branches.** `build/ci/linux-coverage.sh` builds the server and its tester programs with
  clang's source-based coverage (`-DHM_COVERAGE=ON`, a new CMake option), runs the testers and the whole Linux
  regression suite against that server, and writes llvm-cov's line, region and branch figures; a run of the suite
  that did not reach its end is void. On 26 September 2026 on linix they were 49.01%, 47.23% and 38.87% of the
  Linux build (43.35%, 41.38% and 33.82% before the fixtures below were linked, which were measured in a run of
  their own and the counters merged) - a floor, since the Linux suite runs what its REST shims can drive and
  llvm-cov counts every line of a function. The `linux-coverage` job runs it weekly once the owner creates the
  schedule described `coverage` ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md), *The
  coverage schedule*); it has not run on GitLab yet. The Windows measurement of 15 September is in the tree as
  `build/coverage-windows.ps1`, not yet run from it; it stores the suite's resolver over COM before the console
  server starts, as the suite asks since the settings moved into the database. Seventeen class constants the
  server passes to `Format` by reference are defined now (`static constexpr`): six of them kept the instrumented
  build from linking, and the other eleven had the same defect and linked only because the optimiser folded them.
- **Twenty-nine more fixtures run on Linux**, among them DKIM signing, ARC sealing, the OpenPGP keys, message
  recall, shared address books, the IMAP mirror, journaling, per-domain relay and the webmail's footers: each
  already compiled against the REST shims and ran clean on linix, and none was linked. `API/RestApiRouteSweep` is
  new, in both suites: every administrative operation under `/api/v1/` that the server's OpenAPI document
  describes is refused, with 403, to an account without a role, and every read is answered to an auditor without
  a server error. The Linux shims give `Domains.ItemByName[...]` and `DistributionLists.ItemByAddress[...]` as
  indexers, and `Paths.Combine` reads a relative path written the Windows way as one on Linux. The `linux-suite`
  job, which the fixtures lengthen by about 20 minutes, has the runners' four hours now rather than three, and its
  test filter is a variable of its own, `HM_LINUX_SUITE_FILTER`, which the job refuses, so a variable meant for
  another job cannot make the gate run part of the suite.
- **The maintainers' tools**:
  - `build/scan-secrets.py`, which the git hooks and the landing script run: the directory its scanner's container
    writes to is inside a private (0700) directory, locally and over ssh. 6.3.4 made the whole directory 0777, so
    that on a POSIX machine another account could, while a scan ran, read the lines being committed, plant a link
    the writes followed, or put an empty report in place of the scanner's (a Semgrep finding accepted for 6.3.4 as
    a real defect of low severity). And a scan that did not run to its end no longer passes: gitleaks 8.30.1
    writes an empty report and exits 0 when the `git log` it reads fails, so a `--range` naming a commit the
    repository does not have scanned nothing and said nothing was found. Such a range is now refused before any
    scanner is asked, and an empty report is clean only when gitleaks also exited 0 and logged no error.
  - `build/ci/repo-hygiene.py`'s Token-Permissions reads CI rules as GitLab does - every alternative of a
    condition, a rules list at any indentation, tags and `extends` in every form, templates and includes, and a
    job's rules from the last of its `extends` parents, which GitLab merges last over first - and refuses what it
    cannot read; its `--self-test` holds it to 33 cases. Its note on each PostgreSQL password written in
    `.gitlab-ci.yml` names the job whose service container it is.
  - New checkers: `build/check-idl-case.py` fails a COM name that `hMailServer.idl` spells two ways - a type
    library keeps one spelling of a name whatever its case, so a new member spelled like an existing one would
    rename it for every early-bound client - with the seven that shipped that way listed and why;
    `build/check-db-settings-coverage.py` (under Administration). `build/check-deck-parity.py` reads a member
    whose help string holds a `]`, and `build/ci/security-findings.py --reasons` writes the reason each finding
    is known rather than fixed into the baseline.
  - The installer smoke test takes an upgrade that writes `[Security] AdministratorPassword` again as a PBKDF2
    hash, which is the documented upgrade of an older hash, and still requires every other `[Security]` key as it
    was. And it now makes the checks [SecureDefaults.md](hmailserver/docs/SecureDefaults.md) lists for an
    installation on a clean machine: after a new installation, the certificate made at installation (its record,
    its two files, a key only SYSTEM, the administrators and the service can read, RSA 2048 for 825 days naming
    the host), STARTTLS on ports 25, 587, 110 and 143, authentication refused without TLS from the *Internet*
    range and not from *My computer*, the audit rows and the setup checklist; after an upgrade, the ports and
    ranges exactly as they were. Against the signed 6.3.4 installer on a clean Windows Server 2025: a new
    installation passed 59 checks and an upgrade from 6.3.3 73, none failed. The run script follows the VM to the
    address its DHCP lease moved to after a snapshot revert.
- **The CI**: `secret_detection` failed `master`'s first pipeline after 6.3.4 on the 24 vetted test keys and
  fixtures, because a pipeline started by a push scans the checkout as files and names a finding by its path, not
  its commit; `.gitleaksignore` now names each of them that way too. The `static-analysis` and `semgrep`
  baselines are written again from runs over the release's tree in the jobs' own images - 1,165 and 122 known
  findings, 264 and 37 of them with the reason each is known rather than fixed, and every finding no scan reports
  any more removed - and a clang-tidy that did not finish on a file (an abort, a kill) is run again alone and
  fails the job if it still does not finish, where before the file's findings read as fixed and the job passed. The package builds no longer read `$HM_PIPELINE`, which
  in a pipeline where the variable was defined would have refused every build and left GitLab making no pipeline
  for `master`, a batch or a tag; `build/check-release-chain.py` now reads the four package files and fails such a
  rule.
- **The documents, corrected**: 6.3.4's own notes - it was published on 27 September, not 26; the security
  exceptions file accepted 455 findings at the release, not 169; and `ScriptAllowedObjects` first exists in
  6.2.25, so before it an event script could create any COM object. `ServiceAccountName` and
  `ServiceAccountPassword` are no longer used, though still loaded
  ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)); the update feed's
  site has served over HTTPS since 26 September 2026 ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)); an
  inbound webhook's URL writes an international domain name in its `xn--` form
  ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)), and before 6.3.4's fix the check let a URL outside
  printable ASCII through on Linux and Windows alike, where 6.3.4's notes said Linux refused it; and
  CONTRIBUTING.md's schema-upgrade gate and ContinuousIntegration.md's list of what is still done by hand say what
  the jobs do. Two of these, ServiceAccount.md's and UpdateFeed.md's, were found by the check of the wiki's copies
  ([GitLab issue #25](https://gitlab.com/Progressiverobot/hmailserver/-/issues/25)).

Full notes: [the 6.3.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.5).

---

## [6.3.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4) - 2026-09-27

Everything that landed on `master` after the `v6.3.3` tag, between 15 and 26 September 2026 ([compare `v6.3.3` with
`v6.3.4`](https://gitlab.com/Progressiverobot/hmailserver/-/compare/v6.3.3...v6.3.4)). The schema moves from **6040
to 6104 in 64 steps**. Most of what is new is off until an administrator turns it on; what changes on upgrade
without a switch comes first. The operator documents named below are in `hmailserver/docs`, which has 90 more of
them than it had at 6.3.3. This release carries an Authenticode signature and no Sigstore bundle, so every server
installs it by hand: a live update of any version that is offered it says it cannot be verified and downloads
nothing ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)). Its `hmailserver-6.3.4-SHA256SUMS.txt` names every
file of the release but itself and its signature, and is signed with the release tag's SSH key, `hmailserver-6.3.4-SHA256SUMS.txt.sig`;
SECURITY.md, "Verifying a release", has the two commands that check a download against it.

### What changes on upgrade without a switch

1. **The database is the settings store, and the `[Settings]` section of `hMailServer.ini` is its copy** (schema
   6042). At the first start the section's keys are taken into the database, with nothing to run by hand; a line
   edited after the old server last started, including one edited to prepare the upgrade, is taken for an edit and
   put back, so make such a change after upgrading. From then on a line edited in the file is ignored in favour of
   the stored value, named in the error log as `HM5804`, and put back; deleting a line no longer returns a setting
   to its default. Under a service account that cannot write `hMailServer.ini` - `NT SERVICE\hMailServer`, a new
   installation's - the copy is not refreshed and the database's values are used, said once a start in the
   application log. A setting is changed in the Control Panel, over COM (`Settings.SetIniSetting`), over REST (`PUT
   /api/v1/settings/ini/{name}`), or on Linux with `hmailserver --set-setting NAME=VALUE`. A `[SettingsOverride]`
   section in the file is the deliberate way round a database that cannot be read, and is named as `HM5807` at
   every start. No new key is added to `[Settings]` from now on: a new setting is a row in `hm_settings`.
2. **Most saved settings take effect at once.** Every `[Settings]` key now says when a save applies it - 211
   at once, 28 at a reinitialise, 2 at a restart - and the Control Panel, the Control Deck, COM and REST list
   the ones still waiting ([SettingsApply.md](hmailserver/docs/SettingsApply.md)).
3. **SPF is RFC 7208**, ported from upstream PR 645, in place of the RFC 4408 library the server had carried.
   127.0.0.0/8 and `::1` no longer pass unconditionally before any policy is read, so where the SPF test or
   DMARC runs for a loopback client, mail from a domain whose policy does not name the address is judged like
   any other; a content filter that re-injects mail should authenticate or connect from an IP range with
   anti-spam off. SPF lookups follow the server's own resolver settings on Windows, and work on Linux, where
   every check had been TempError. Every result is written under its own keyword; only a fail scores, as
   before.
4. **The audit trail is on** (schema 6043): an administrative change over COM or REST writes a row, kept
   until a retention is set, with secrets recorded as `(changed)`. **Alerts are on**, with seven of their
   eleven conditions enabled, `backup.failed` and `certificate.expiring` among them, but nothing is mailed
   until `AlertRecipient` and `AlertSenderAddress` are set
   ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md)).
5. **Compromised-account detection starts in watch mode** (schema 6071): it builds each account's baseline
   from authenticated submissions and raises `account.compromised` when a submission has the shape of a
   takeover, and holds nothing until `compromise_detection_mode` is set to `enforce`
   ([CompromisedAccounts.md](hmailserver/docs/CompromisedAccounts.md)).
6. **A destination that asks this server to slow down is given a pause.** A 421, or a 4xx with one of the
   providers' rate-limit codes or phrases, holds deliveries to that destination alone for
   `OutboundBackoffMinutes` (2 by default, doubling to `OutboundBackoffMaxMinutes`, 60); 0 turns the back-off
   off. A wait this server chose no longer spends a message's retries until the message has been queued
   for longer than its retry schedule spans, so a destination that never stops asking still ends in a bounce
   ([OutboundShaping.md](hmailserver/docs/OutboundShaping.md)).
7. **A backup carries what an account keeps beyond its mail** - the address book, the webmail's preferences,
   scheduled sends, file links, S/MIME keys, calendars, password history, the first-contact memory and every
   message's keywords - and a restore puts them back, with each folder's access grants. Passkeys, calendar and
   address-book shares, deleted mail waiting to be recovered, messages that expire, IMAP annotations and the
   out-of-office memory are not carried, each for the reason `AccountStores.cpp` gives. An archive written by
   6.3.3 or earlier restores exactly as it did, without what it never held.
8. **The out-of-office memory survives a restart** (schema 6060): who has been answered is kept in a table,
   so a restart no longer sends every correspondent the reply again.
9. **Addresses collected from what a person sends are suggestions, not contacts.** They appear under Other
   contacts in the webmail, and CardDAV serves only the contacts a person kept.
10. **In the container image, the environment wins.** `HM_SETTING_<key>` and `HM_PROPERTY_<name>` (with `_FILE`
    variants) are stored at every start, and a setting the environment manages is refused to the other
    interfaces until a start without the variable
    ([EnvironmentConfiguration.md](hmailserver/docs/EnvironmentConfiguration.md)).
11. **`GET /api/v1/me/search` goes on from where it stopped.** A search with more hits than `limit` answered
    `complete: true, more: true` and could not reach the rest; it now answers `complete: false, more: true`
    with `next_before_id`, which `before_id` continues from, and `complete` is false with a
    `next_before_id` whenever an answer is not the end. Hits are newest first across every folder by message
    id, where they were folder by folder ([Performance.md](hmailserver/docs/Performance.md)).
12. **IMAP answers as the RFCs say, where Dovecot's imaptest found it did not.** A client that relied on the old
    answers sees these: a mailbox that does not exist, or a right the account lacks, is refused `NO` rather than
    `BAD`, and a failed SELECT or EXAMINE leaves no mailbox selected; an unknown LIST selection or return option is
    `BAD`; every `BODY[<section>]` without `.PEEK` sets `\Seen`, `BODY[HEADER]` and `BODY[1.MIME]` included; SORT
    uses every criterion, REVERSE reverses only its own, ties go to sequence order and an ASCII string folds ASCII
    letters only (a string beyond ASCII is compared as before), and THREAD ORDEREDSUBJECT and REFERENCES build the
    RFC's trees, so both can order messages differently; a session that is the first to be told of new messages
    counts them as `\Recent` (RFC 3501 2.3.2), where it said `* 0 RECENT`; and an APPEND stores the literal byte
    for byte, where it used to add a CRLF, so RFC822.SIZE of a newly appended message is two octets smaller than
    the same message appended before.
13. **MySQL and MariaDB tables become utf8mb4** (schema 6102). Every table is converted to utf8mb4 under
    `utf8mb4_general_ci`, InnoDB and `ROW_FORMAT=DYNAMIC`, and so is the database's default. Before it, 106 of the
    123 tables were utf8, three bytes a character, so an emoji or a CJK Extension B character in a contact, a
    subject in the message index, a signature or a log line was refused with error 1366 or made a question mark.
    The step rebuilds every table, which takes time on a large database; it needs MySQL 5.7.9 or MariaDB 10.2.2 or
    later and the ALTER privilege on the database, and a step stopped part of the way is finished by running it
    again. So do steps 6041 and 6101 before it, and each now opens with a probe table that needs the same key size,
    so on an older server the upgrade stops at 6041 with error 1071 having changed nothing. Fixed: those two steps
    made their first tables before the index that failed, and MySQL commits DDL at once, so they stopped half done
    and every later run failed on a table already there; a database an earlier build left that way, at 6040 (or,
    after a change of database server between the two steps, at 6100), is put back by hand with the statements
    Upgrading.md gives. Two rows the old collation told apart and the new one does not - on MariaDB 11.8, `straße` and `strase`
    in one address book; on MySQL 8.4, two names that differ only in an emoji - stop it with error 1062 until one
    of them is removed. SQL Server, SQL Server Compact and PostgreSQL only move the version
    ([Upgrading.md](hmailserver/docs/Upgrading.md),
    [MigratingDatabaseBackend.md](hmailserver/docs/MigratingDatabaseBackend.md)).
14. **The upgrade alters the message table.** Schema 6101 adds two columns to `hm_messages`, the table that has a
    row for every stored message: the account an authenticated session submitted a message as, and the domains that
    passed DMARC and SPF when it arrived, recorded as properties of the message and never as headers. On a large
    store the step is not instant on every backend.
15. **The database connection is encrypted when the database server offers TLS.** MySQL and MariaDB through MariaDB
    Connector/C - the client bundled on Windows and the one Ubuntu packages - had never used TLS, whatever the
    documents said; with nothing configured they now use it whenever the server offers it, without checking the
    certificate, and connect without it when it offers none. SQL Server through MSOLEDBSQL 18 and SQL Native Client
    now encrypts the whole session, and tries once more without encryption when the failure reads as TLS; through
    SQLOLEDB, which the server picks when MSOLEDBSQL 18 or later is not installed and no failover partner is set,
    only the login is encrypted unless `TlsMode` is `require` or `verify-full`. PostgreSQL is unchanged. Two
    `[Database]` keys choose otherwise: `TlsMode` (`prefer`, the default, `require`, `verify-full` or `disable`)
    and `TlsCaFile`. The application log says at every start what the connection got, and `--check-database`
    makes one connection and says the same
    ([DatabaseTls.md](hmailserver/docs/DatabaseTls.md)).
16. **The administrator password is hashed again at sign-in.** The Windows installer wrote it as an unsalted MD5
    digest that nothing ever replaced. The first successful administrator sign-in - the Control Panel, COM, REST,
    `hmctl` or the Control Deck - writes an MD5 or salted SHA-256 value, or a PBKDF2 hash with fewer iterations
    than `PasswordHashIterations`, again as PBKDF2; a browser session made under the old hash ends once. A file the
    service cannot write keeps the old value and says so once (`HM7494`). An upgrade seals nothing else: see
    Encryption ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
17. **A COM change to a shared configuration object takes effect at `Save()`.** For routes and their addresses,
    incoming relays, DNS black lists, SURBL servers, remote-domain policies, server messages, blocked attachments,
    SSL certificates and groups, a `put_*` now writes a copy held by the COM object, not the item every session is
    reading, so it is not in effect until `Save()`, and one never saved changes nothing; an object read from the
    collection before the save keeps what it read. A script that set one of these and relied on it without saving
    must now save. The old way let a session read a string while it was being freed, the use-after-free under the
    heap-corruption abort below; a save whose re-read fails is reported as error 7390
    ([Upgrading.md](hmailserver/docs/Upgrading.md)).
18. **Mail labelled ISO-8859-1 is decoded as windows-1252**, as the WHATWG Encoding standard and every mail client
    read it, and ISO-8859-2 to -9, -13 and -15 by their own code pages. There was no ISO-8859 name in the server's
    table at all: a Western Windows server decoded Latin-1 right by accident and decodes it as before, one with
    another ANSI code page now decodes it right too, and on Linux Latin-1 had become replacement characters.
19. **A restore lists the archive before it extracts anything, and refuses an unsafe one whole**: an entry with an
    absolute path, a `..` segment or a link with somewhere to point is named in the backup log and as `HM7540`, and
    nothing is extracted. No backup this server writes has such an entry. The Windows installer's 7-Zip is 26.03,
    fetched from 7-zip.org, in place of 19.00, which predates the fixes for CVE-2023-31102 and CVE-2025-55188
    ([Backup.md](hmailserver/docs/Backup.md)).
20. **Event scripts: a rule's Run function action names a plain function.** A name that is not ASCII letters,
    digits, `_` and `$`, not starting with a digit, is refused as `HM5018` each time the rule fires, in either
    Windows language, VBScript or JScript, where it used to be written into the script's call as it was. On Linux,
    whose JavaScript engine looks a handler up by name instead of building a call, such a name is never evaluated
    and finds no function: nothing is run, and the ERROR log has a Script Error line, not `HM5018`
    ([EventScripts.md](hmailserver/docs/EventScripts.md)). On a JScript server every value a
    handler is given - a password, a remote UID, an error text - now arrives exactly as it was, backslashes
    included (see the fix under Security and identity).
21. **A domain can no longer be named as a directory the server keeps its own files in.** A domain's mail is kept
    in the directory named after it under the data directory, beside the server's stores, so a single-label name
    such as `protected`, `confidential`, `recoverable`, `discovery`, `quarantine`, `files`, `lists` or `blobs` (in
    any case), or `temp`, `events` and `database`, which the Linux packages put there, is refused for a new domain
    and for a rename over COM and REST. A domain that already has one is still saved, and is named as `HM7597` at
    every start and as `domain_name_reserved` on the first-hour checklist: rename it
    ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)).
22. **A one-time code is taken once.** Over COM and over REST, a TOTP code the server has accepted is refused if
    it comes again, for the administrator and for an account alike (see the fix under Security and identity). A
    script that sent the administrator's password and a code with every REST request signs in once instead -
    `POST /api/v1/session`, and the `hmailsession` cookie from there - or uses an API key; otherwise only its first
    request in each half minute is accepted. The others are refused with 401 and are not counted as wrong codes, so
    such a script does not lock the administrator out. Somebody asked for a code twice within half a minute - to sign in to
    the Control Deck and then to register a passkey, say - waits for the authenticator's next one
    ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
23. **The outbound relay sends its OAuth2 token over TLS only.** A route or the SMTP relay whose host is on
    `OutboundOAuth2Hosts` and whose connection security is none - or STARTTLS optional against a server that does
    not offer it - sent `AUTH XOAUTH2` with the server's own token in the clear. Such a delivery now sends
    nothing in its place, fails with a temporary `450 4.7.0`, and the error log says to set the security to TLS
    or STARTTLS required (`HM5912`); the message waits for the retries the route allows. Exchange Online offers
    STARTTLS on 587, so a route set up as Microsoft documents it is not affected.
24. **A password hash stored before 6.3.4 of a password with a character above U+00FF is refused**, to its own
    password too, and the application log names the account: that hash was of NUL bytes or question marks, and
    any other password of that form opened it (see the fix under Security and identity). An administrator sets
    such a password again, and soon: until then SCRAM-SHA-256, whose client derives its key itself, still opens
    such a PBKDF2 hash to the NUL bytes (Windows) or question marks (Linux) it was made over, which the server
    cannot tell from the password. Every ASCII and Latin-1 password signs in as before.

### Delivery and SMTP

- **Remote-domain policies** (schema 6045): for one remote domain or a pattern, the outbound TLS it demands
  (encrypted, verified or DANE - a delivery that cannot meet it is deferred, never sent in the clear), TLS
  required of its senders, size, connection and rate limits, whether out-of-office replies, forwards and Sieve
  redirects may go to it, and a backup-MX recipient callout
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Throttling per destination and per tenant**: besides the back-off above, `MaxOutboundConnectionsPerDestination`
  and `MaxOutboundConnectionsPerTenant` (both off) so that one busy destination or one hosted domain cannot
  hold every delivery thread; the state is at `GET /api/v1/queue/shaping`.
- **IP warm-up** (schema 6093): a schedule for a new outbound address holds it to a daily ceiling of recipients
  at each large provider, rising along a standard, gentle or fast plan; nothing is held until a schedule exists
  ([IpWarmup.md](hmailserver/docs/IpWarmup.md)).
- **A send API for applications** (schema 6089): an API key becomes a sender, with an address of its own and
  ceilings a minute and a day; `POST /api/v1/send` sends and `GET /api/v1/send/{queue_id}` says what became of
  each recipient ([SendApi.md](hmailserver/docs/SendApi.md)).
- **DKIM keys that rotate** (schema 6048): a policy per domain publishes a second selector, switches once the
  world has seen it and retires the old key, with an alert when a rotation stalls; off on every domain
  ([DkimKeyRotation.md](hmailserver/docs/DkimKeyRotation.md)).
- **What a domain does to a message** (schema 6044): an external-sender tag on the subject or as a header, a
  first-contact note, and a disclaimer on mail leaving the organisation, all off; the domain's signature and
  disclaimer now also go on mail sent from the webmail and its send-later
  ([DomainTransforms.md](hmailserver/docs/DomainTransforms.md), [Signatures.md](hmailserver/docs/Signatures.md)).
- **Restricted delivery** (schema 6084): accounts, or a whole domain, that may exchange mail only with named
  domains ([RestrictedDelivery.md](hmailserver/docs/RestrictedDelivery.md)).
- **Message recall within the server** (schema 6085): the sender withdraws an unread message delivered to
  accounts on this server ([MessageRecall.md](hmailserver/docs/MessageRecall.md)).
- **Journaling and dual delivery** (schema 6075): a copy of a domain's or one account's mail sent to an
  address, as a journal report or as the message itself ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Quota policy** (schema 6061): a domain's own warning threshold and text, a grace past the limit for
  incoming mail, and a share of the limit at which the account can no longer send
  ([QuotaPolicy.md](hmailserver/docs/QuotaPolicy.md)).
- **Out-of-office policy** (schema 6060): a window of days before the same sender is answered again, a text
  for colleagues and replies to colleagues only ([OutOfOffice.md](hmailserver/docs/OutOfOffice.md)).
- **Mail in, to a webhook** (schema 6100): an address, or a whole domain this server hosts, is routed to a URL, and
  the mail that arrives for it is posted there parsed - as `multipart/form-data` in SendGrid's inbound parse field
  names or as one JSON document - signed with the outbound webhooks' HMAC-SHA256 recipe and retried on their
  schedule; when the attempts run out a route keeps the message to be downloaded or retried, or returns it to its
  sender. A route may deliver to the mailbox as well. Link-local and cloud metadata addresses are refused to
  everybody, an API key may name only a public address, and plain `http` is taken only to this machine. A URL is
  printable ASCII, and anything outside it is refused on both platforms - fixed before release, when Linux and
  Windows alike put such a URL into the request line as it was. Routes are
  made over REST (`/api/v1/inbound-webhooks`) and on the Control Deck, under a new permission,
  `manage_inbound_webhooks`, that only `serveradmin` holds; there is no COM member and no Control Panel page.
  Nothing is on until a route is made ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
- Fixed: an outbound bind address of one address family no longer stops delivery to the other (GitHub issue
  276); `SMTPDeliveryBindToIP` may name one address of each family, separated by a comma.

### Anti-spam, anti-virus and mail flow rules

- **Greylisting hardened** (schema 6072; GitHub discussions 281, 282 and 284): the triplet key no longer
  truncates long addresses; a sliding wait on an early retry and an abuse lock, each off by default; and a
  bypass list whose entries may require SPF, DKIM or DMARC to pass
  ([Greylisting.md](hmailserver/docs/Greylisting.md)).
- **SMTP policy a score cannot express** (schema 6073; GitHub discussions 283, 285 and 286): scores of their own
  for SPF softfail, permerror and temperror, a temperror deferral, a ban or refusal for a null sender with many
  recipients, and a domain white and black list by envelope domain or HELO; all off
  ([SmtpPolicy.md](hmailserver/docs/SmtpPolicy.md)).
- **Allow and block lists and quarantine digests** (schema 6046): senders or domains allowed or blocked for the
  server, a domain or one reader, covering the From header's address as well as the envelope sender, and URL
  entries for the links in a message; held mail summarised daily to the person it was
  held for, once `QuarantineDigestBaseUrl` is set and a domain or an account turns the digest on (off by default)
  ([SenderListsAndDigests.md](hmailserver/docs/SenderListsAndDigests.md)).
- **Mail flow rules** (schema 6063): a domain's own rules, a condition on an attachment's name, and actions to
  add a disclaimer, reject with a text, require TLS, quarantine or warn the sender; and an **outbound content
  policy** - card numbers, IBANs, account numbers and attachment types, to external recipients - as conditions
  of the same rules, with no second engine ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- **Impersonation protection** (schema 6087): protected persons and partner domains per domain, judged on mail
  from outside; off on every domain ([ImpersonationProtection.md](hmailserver/docs/ImpersonationProtection.md)).
- **Attachments opened somewhere safe** (schema 6091): an outside message whose attachment is an executable, a
  script, a shortcut, an archive, a disk image, an office document or a PDF is held until a sandbox service the
  administrator names answers;
  off ([Sandbox.md](hmailserver/docs/Sandbox.md)).
- **Spam traps** (schema 6058): an address that was never real refuses like an unknown user, bans the connecting
  address and, when its domain's SPF passes for that address, puts the envelope sender on the block list
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Country blocking** (schema 6052): connections and sign-ins judged by country from a MaxMind database the
  administrator supplies, with loopback and private ranges exempt and a missing database meaning allow
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Sealed mail** handled honestly: a message the server cannot read is no longer gutted by the
  blocked-attachment list or given a clean verdict by a scanner that saw only ciphertext
  ([SealedMail.md](hmailserver/docs/SealedMail.md)). "Cannot read" is judged from the body, never from the
  `Content-Type` label: only a message that really is S/MIME enveloped or signed-data, or PGP/MIME of exactly a
  version part and armoured ciphertext under a session key, named as clients name them or not named at all, is
  spared the blocked-attachment list, the anti-virus strip action, eDiscovery's body search and the rules' body, `content`
  and `sensitive_content` conditions. Fixed before release: as first written, a `multipart/encrypted` header any
  sender could add switched all of those off for the ordinary parts under it, `invoice.exe` included. The
  detached signature of a signed message is likewise left alone only when it is one - the second of the two
  parts of a `multipart/signed`, called and made as a signature is; as first written, any part anywhere that
  typed itself `application/pkcs7-signature` or `application/pgp-signature` was spared the list, whatever it
  was called.
- Fixed: a message held by this server's own outbound shaping - a destination's back-off, a rate or connection
  ceiling, the tenant share, the IP warm-up - no longer sends a server or domain rule's forward, copy or reply,
  or calls its script function, once per held pass. Each pass re-ran the rules, and each queued the new message
  again, so a message held every few minutes for as long as its retry schedule spans sent one copy per pass;
  those actions now run once per counted attempt. An account's own rules, which run once for their recipient,
  are not held back by this, and an account rule's warn_sender now goes when its recipient is reached on a later
  attempt too - it was skipped, as the server's once-per-message notice is
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md), [OutboundShaping.md](hmailserver/docs/OutboundShaping.md),
  [OutboundContentPolicy.md](hmailserver/docs/OutboundContentPolicy.md)).
- Fixed: a mail flow rule's forward, copy, reply or notice to the sender, and an account's own forwarding, no
  longer wait up to a minute in the queue. Each is queued from inside the delivery of the message that caused
  it, after the delivery manager last read the queue, and the manager was not told of it, so it sat there until
  something else woke the manager, a minute later at the latest.
- Fixed: one domain's mail flow rules no longer act on another domain's recipients. A domain's rules ran on the
  whole of every message with a recipient in the domain, and on every message whose envelope sender claimed the
  domain, authenticated or not, so one tenant's delete or reject removed a message for another tenant's
  recipients, and its header, route or bound address applied to their copies. The sending domain's rules - the
  submitting account's domain, and the envelope sender's where its account submitted the message, DMARC passed
  for it, or the message reaches no other domain's mailboxes - still act on the whole message; a recipient
  domain's act on the whole message only when every recipient is its own, and otherwise take its own recipients
  off (a quarantine keeps a copy for them, and a reject mails a hosted sender a notice naming them), copy to them
  alone and leave the message's headers, body and routing alone
  ([MailFlowRules.md](hmailserver/docs/MailFlowRules.md)).
- Fixed: a custom virus scanner whose command line holds a bare `%FILE%` was handed the message file's path
  cut at its first space - and the default data directory, under `C:\Program Files`, has one - so it scanned no
  file, answered with an exit code other than the virus one, and the message was delivered as clean. A `%FILE%`
  outside double quotes is now replaced by the path in quotes; one already inside them, `"%FILE%"` as the
  documented examples write it, is replaced as before. ClamWin's database and temporary folders lose a trailing
  backslash, which escaped the closing quote of `--database="..."` and took the file to scan with it.

### IMAP, POP3 and the webmail

- **Twelve webmail features**: installs as an app with the unread count on its icon, shares from other apps and
  `mailto:` links; contacts imported and exported as vCard or CSV; saved searches as folders; the reader's own
  time zone; Undo on three more actions; the tracker notice counts what it blocked; swipe actions and snooze
  presets; three stars (`$Star2`, `$Star3` on top of the flag); rows coloured by a rule; `@`-mentions completed
  from the address book; a formatted signature with an image; and Report phishing beside Junk.
- **Arabic, Hebrew and Persian**, with the layout mirrored: 23 catalogues besides English.
- **Recover deleted items** (schema 6068): a deleted message is kept aside for `recover_deleted_days` (off by
  default) and put back by its owner ([RecoverDeleted.md](hmailserver/docs/RecoverDeleted.md)).
- **A message that expires** (schema 6069): the recipient gets a link, not the message, with an optional
  passcode ([ConfidentialMessages.md](hmailserver/docs/ConfidentialMessages.md)).
- **Offline mail** for the last N days, kept in the browser under a ceiling the administrator sets
  (`ReaderOfflineDaysMax`, 30) ([OfflineMail.md](hmailserver/docs/OfflineMail.md)); **Web Push** for new mail
  with the page closed (schema 6054, off).
- **A second account in the same page** ([SecondAccount.md](hmailserver/docs/SecondAccount.md)); the reply card
  above the conversation as a choice (GitHub discussion 266); auto-delete of Trash and Junk and auto-archive of
  the inbox as the reader's own choices.
- **Important, and sections of the inbox**, and emoji, pictures and tables in compose
  ([ImportantMail.md](hmailserver/docs/ImportantMail.md), [FormattedMessages.md](hmailserver/docs/FormattedMessages.md)).
- **Masked addresses** (schema 6067), **sending as another verified address** (schema 6066), managing
  subscriptions, and a sender's BIMI logo as its avatar for mail that passed DMARC
  ([MaskedAddresses.md](hmailserver/docs/MaskedAddresses.md), [SendAs.md](hmailserver/docs/SendAs.md),
  [BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **Calendars**: CalDAV (RFC 4791) beside CardDAV, on an iCalendar (RFC 5545) reader and writer of this
  project's own with bounded recurrence expansion (schema 6041); the webmail's calendar and tasks pages, several
  calendars with colours and shares (schema 6064), invitations by mail (iMIP), and free-busy, group and
  delegate shares (schema 6086) ([Calendar.md](hmailserver/docs/Calendar.md)).
- **Contacts**: contact groups, the organisation's directory of a domain's people and lists (schema 6078), and
  shared address books handed to a colleague or a whole domain (schema 6079)
  ([Contacts.md](hmailserver/docs/Contacts.md), [Directory.md](hmailserver/docs/Directory.md),
  [SharedAddressBooks.md](hmailserver/docs/SharedAddressBooks.md)).
- **Rooms and equipment** (schema 6101): an account can be a room or a piece of equipment (`kind` over REST, the
  Deck's account editor, `Account.Kind` over COM), which answers the invitations it is sent by itself, at delivery,
  by its booking policy - who may book, a booking window (180 days as shipped), a longest booking, working hours,
  conflicts and recurring meetings - and leaves what it does not decide to its delegates. It believes an organiser
  only from what the server recorded when the message arrived - the account that submitted it, or a DMARC or SPF
  pass for an outside domain the policy names - never from a header. A resource cannot be signed into. The
  directory lists rooms with their capacity, and CalDAV's room finder searches them. The policy is at
  `/api/v1/accounts/{address}/booking-policy`, with no COM or Control Panel page; nothing changes until an account
  is given a kind ([RoomsAndEquipment.md](hmailserver/docs/RoomsAndEquipment.md)).
- Fixed: on SQL Server and SQL Server Compact a room's record of whom it accepted a booking from was keyed on the
  UID under a collation that ignores case and width, so a booking whose UID differed from another's only that way
  replaced the other's record, and a delegate's later decline of the first booking told its organiser nothing. The
  UID is now compared exactly on every database, so the first booking keeps its record; the second, which the
  unique index there takes for the same UID, goes unrecorded, and the application log says that a delegate's later
  answer about it will tell nobody.
- **Delegated mailboxes named in the Autodiscover answer**, in Exchange's `AlternativeMailbox` element and only
  to a request signed in as the delegate. Outlook reads that element for an Exchange account and not for an IMAP
  one, so for IMAP clients the mechanism that works is still the `#Users` namespace, which Thunderbird and
  Outlook's IMAP code show by themselves.
- Fixed: a heap-corruption abort under concurrent IMAP `LIST` and folder changes on the same account; every
  loop over a shared collection now iterates a locked copy.
- Fixed: an IMAP command pipelined behind an `APPEND` with a non-synchronising literal was never answered, which
  stopped `mbsync` after one message.
- Fixed: a heap-corruption abort when two IMAP sessions on one mailbox met on the same message's keywords - a
  `STORE` of `$Forwarded`, `Junk` or a label beside a `FETCH`, a `SELECT` or another `STORE`, which is what two
  of Thunderbird's connections, or a phone and a desktop, do. Every session shares the one message object, and its
  keywords were rewritten with no lock; they are now read and changed under a lock of their own, so the abort
  cannot happen. 6.3.3 had it too.
- Fixed: two changes to one message's flags at the same moment - two IMAP `STORE`s from two sessions, a `STORE`
  beside a `FETCH` that sets `\Seen`, or beside the webmail's flag and label routes - could each write back the
  flags and keywords it had read before the other's change, so one change was lost, in memory and in the
  database. The read, the change and the write are now one step under a lock of the message's own, and each flag
  is set and cleared on its own bit. `STORE ... (UNCHANGEDSINCE n)` is tested in the same step; a `STORE` refused
  for too many keywords leaves the message as it was rather than half changed in memory and never written; and a
  flag change no longer writes back a `\Recent` another session has just claimed. As old as `STORE`.
- Fixed: an IMAP `APPEND` whose client went away after sending a message's octets and before the command's
  final line break left that message's file in the data directory for good, with no row, counted by no quota,
  and - with `MessageStoreCompression` on - holding its references to shared parts. An authenticated user could
  fill the volume by repeating it until the server refused mail for everyone. The file is now removed with the
  connection, and one left by a server that stopped in the middle of such an `APPEND` - a crash, a kill, a power
  cut - is removed when the server next starts: every such file has a marker in the temp folder's `IMAPAppend`
  directory until its row is saved, and the file of a marker no row answers goes
  ([MessageStore.md](hmailserver/docs/MessageStore.md)). 6.3.3 had it too.
- Fixed: an IMAP `APPEND` into a folder shared from another mailbox (`#Users`) was measured against the quota
  of the user appending, not of the mailbox that stores it, so a delegate with the insert right could push the
  owner past the limit, and a delegate over their own limit was refused an append to a mailbox with room. It is
  now charged to the owner, as `COPY` and `MOVE` already were. 6.3.3 had it too.
- Fixed: an IMAP `FETCH` of a message the server could not read - one in a mailbox encrypted at rest while its
  owner's key is closed, as a delegate meets it, or a damaged one (`HM7350`) - was answered `OK` with an
  `ENVELOPE` of `NIL`s, an empty structure and empty sections, which clients keep by UID for good, and a
  non-`PEEK` section set `\Seen`. It is now answered `NO [UNAVAILABLE]` for a closed key, with nothing of the
  message sent, and said in the debug log only, not the error log: a closed key is not damage. A damaged one is
  left out and the rest of the range fetched, and the command ends `NO`, as for a message lost from an object
  store, so one damaged message does not stop a whole folder's sync at itself. Every part is read before any is
  sent, so a failure can no longer leave an untagged `FETCH` unfinished
  ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- Fixed: nothing bounded an IMAP `LIST` or `LSUB` pattern but the 11 MB one command may hold - a quoted pattern
  goes on over the lines a literal joins - and matching it against each folder cost at least its length on the
  threads every protocol shares: 6.3.3 copied the whole pattern twice for each, and this release's matcher, which
  finds a wildcard anywhere in a pattern, cost its length times the folder name's. So one authenticated client's
  pattern of `*` megabytes long could hold those threads. A run of wildcards is now matched as one and a pattern
  that cannot match stops at once, and a pattern longer than 8,192 characters with its reference - longer than
  any name a folder can have - or more than 32 patterns in one `LIST` is answered `BAD`. `LIST` and `LSUB` also
  read a reference or pattern sent as a literal (`{n}`), as RFC 3501 allows, and the same bound holds for it: they
  matched the literal's length as the pattern, so such a command did not list what it asked for. 6.3.3 had that
  too.
- Fixed: a message whose `Date` header held a backslash - one at its end, say - broke the IMAP `ENVELOPE` it was
  sent in: the backslash escaped the date's closing quote, so a client read the subject and the addresses the
  sender wrote as part of the date and then as the response's own structure. The date's backslashes are left out,
  as its quotes already were. 6.3.3 had it too.
- Fixed: the same for IMAP `BODYSTRUCTURE`: a part's type, charset, file name, boundary, `Content-ID` and
  disposition were put between quotes as the sender wrote them, so a file name ending in a backslash or a
  `Content-ID` holding a quote broke the structure every client parsed, and a folded `Content-ID` ended the
  response line. Their quotes and backslashes are now escaped and a control character is sent as a space; a value
  without any of these is sent as before. 6.3.3 had it too.
- Fixed: on an IMAP connection compressed with `COMPRESS DEFLATE`, what one read inflated past the connection's
  250,000-octet receive buffer was dropped without a word. A few hundred compressed octets of a repeated byte
  inflate to more than that, so such an `APPEND` stored its message cut short, or never finished, and the session
  lost step with the client. The rest now waits and is taken before the connection reads again. 6.3.3 had it too.
- Fixed: a later message of an IMAP `MULTIAPPEND` refused under a `{n+}` literal, or any literal after the command
  had failed, was read and thrown away whatever size it declared, past the 2 GB ceiling too, so a client could
  keep a connection reading for as long as it liked. Declared larger than the maximum message size (and 1 MB),
  it now closes the connection with `BAD [TOOBIG]`, as the first literal already did. 6.3.3 had it too.
- IMAP `COMPRESS DEFLATE` is taken, and `COMPRESS=DEFLATE` advertised, only once the client has signed in; before
  that it is answered `NO`. It was taken in any state, which put the inflater within reach of anyone who could
  open a connection, for no client that compresses before it authenticates.
- Fixed: IMAP `STARTTLS` was taken, and offered, after `LOGIN` and inside a TLS session it had already started,
  where it began a second handshake on the running one. It is a command of the not authenticated state only
  (RFC 9051); it is now answered `BAD` and left out of `CAPABILITY` once TLS is active or the client has signed
  in, as SMTP's `STARTTLS` and POP3's `STLS` already were. 6.3.3 had it too.
- **A folder of a hundred thousand in the webmail**: a page copies one page of the folder, never all of it;
  Oldest reaches the end in one request; a search is bounded by time as well as by files read, spared by
  the full-text index, and goes on from where it stopped across every folder; select-all hands the
  selection to the server (`POST /api/v1/me/folders/{id}/bulk`), which works through it in bounded steps.
  The seeding and timing scripts are in `build/perf/`; the numbers are to be measured on the bench
  ([Performance.md](hmailserver/docs/Performance.md)).

### Security and identity

- **Administrator roles** (schema 6047): `serveradmin`, `domainadmin`, `helpdesk` and `auditor` over 27
  permissions, custom roles, assigned per account and per domain ([Roles.md](hmailserver/docs/Roles.md)).
- **Passkeys and single sign-on** (schema 6077): WebAuthn, and OpenID Connect from the organisation's issuer,
  into the webmail and the Control Deck; **SAML 2.0** beside it (no schema step), with an XML canonicaliser and
  signature verifier of the project's own ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **SCIM 2.0 provisioning** (schema 6096): the identity provider makes, renames, suspends and removes accounts,
  aliases and lists at `/scim/v2` ([Scim.md](hmailserver/docs/Scim.md)).
- **A recovery address and one-time recovery codes**, and self-service password reset (schema 6076)
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Sessions and devices** (schema 6053): every client that signs in is a row its owner and the administrator
  can see and revoke; the session policies start unset, after an upgrade as in a new database, so the
  ceilings the server always had (thirty minutes idle, twelve hours in all) apply until they are set
  ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **A new password checked against published breaches** by k-anonymity, wherever a password is chosen; off by
  default ([PasswordBreachCheck.md](hmailserver/docs/PasswordBreachCheck.md)).
- **Compromised-account detection** (schema 6071), in watch mode as above, with a review queue.
- **Fixed: a password could run code inside the server.** On Windows, with event scripting on, the script language
  JScript and an `OnClientValidatePassword` handler in `EventHandlers.js`, any client that could reach a sign-in -
  SMTP AUTH, POP3, IMAP or anything else that checks an account's password - and name an existing, active account
  could have JScript of its own run inside `hMailServer.exe`, with the rights of the service's account, before it
  had authenticated. That script could create any COM object, `WScript.Shell` among them - before 6.2.25 always,
  from 6.2.25 with `ScriptAllowedObjects` at its shipped `*` - and so run any command the service's account may.
  The server built each call as source text and escaped the apostrophe but not the backslash before it. The same shape was open to a remote POP3
  server through the UID it answers (`OnExternalAccountDownload`), and to a remote mail server through the error
  text it refuses a delivery with and the failed recipient's address (`OnDeliveryFailed`), each on a JScript server
  with that handler; and a rule's Run function action wrote its function name into the call unescaped in either
  language, so whoever may edit a rule, a domain's own administrator included, could do the same. VBScript servers
  were not open through a password, a UID or an error text, and the Linux builds had no script engine. The code was
  there in every release from 6.0.0 to 6.3.3. Every JScript value is now escaped in one helper, and a rule's
  function name must be a plain identifier ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **Fixed: an app password was given the account's private keys, sealed under the account password.**
  `GET /api/v1/me/smime` answered a request made with an app password every own S/MIME key as the webmail
  wrapped it, under a PBKDF2 key from the account password, as 6.3.3 already did; and, new in this release,
  `GET /api/v1/me/zero-access` the zero-access mailbox key's two sealed copies with their Argon2id salt and
  parameters, and `GET /api/v1/me/openpgp/keys` every own OpenPGP secret key. Whoever held a leaked app password -
  a credential for a mail program, which cannot change any of these - could take the copies away and guess the
  account password offline, and a right guess opened the mailbox key and the account. The three reads now leave
  the keys out, with `keys_withheld`, for an app password and for an administrator's support session, so a webmail
  session started with either cannot open them and says so. A zero-access row's Argon2id parameters
  are held to at least two passes over 19 MiB, where 8 MiB and one pass were taken
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: an app password could replace or remove the account's own S/MIME certificate and private key**
  (`PUT /api/v1/me/smime/own`, `DELETE /api/v1/me/smime/own/{fingerprint}`), whose wrapped key the store holds the
  only server copy of, as 6.3.3 already allowed; both are now refused to it with 403, as the same writes to an
  OpenPGP key of the account's own always were.
- **Fixed: an administrator's support session could change the account's keys.** A support session acts as the
  person in the webmail so an administrator can help them, but it could store or remove the account's own S/MIME
  certificate and key and a correspondent's certificate, as 6.3.3 already allowed, and - new in this release -
  store or remove an OpenPGP key of the account's own or a correspondent's, mark a correspondent's key verified or
  accept its change, upload, remove or publish the key the Web Key Directory serves for the address, and switch
  zero access on or off or re-wrap its key: planting what the person's mail is then sealed to, or storing the next
  delivery to a zero-access mailbox in the clear for the administrator it exists to keep out. Every one of those
  writes is now refused to a support session with 403 and `code` `support_session`, as the S/MIME authority's
  enrolment already was; the session still reads them, without the wrapped keys. A correspondent's S/MIME
  certificate, and the published OpenPGP key and its switch (`PUT`/`DELETE /api/v1/me/openpgp`,
  `PUT /api/v1/me/openpgp/publish`), are now refused to an app password too, as a correspondent's OpenPGP key
  always was - a leaked app password could otherwise have published its own key for the address. A support
  session can no longer remove or rewrite the webmail's note that an administrator opened the mailbox
  (`support_last`) or the consent (`support_allowed`) in `PUT /api/v1/me/preferences`, and an app password can
  no longer give that consent ([Roles.md](hmailserver/docs/Roles.md), [OpenPGP.md](hmailserver/docs/OpenPGP.md),
  [ZeroAccess.md](hmailserver/docs/ZeroAccess.md)).
- **Fixed: a person could collect any Microsoft 365 mailbox with the server's own token.** On a server set up to
  collect Microsoft 365 mail with its own OAuth2 app token (`OutboundOAuth2*` or `OutboundOAuth2FixedToken`), an
  external account for a host on `FetchOAuth2Hosts` logs in with that token as whatever user name it gives. The new
  Other accounts page (`/api/v1/me/fetch-accounts`) let any person add one for `outlook.office365.com` naming
  another person's address, or rename the user on one an administrator had set up, and so copy - and with days to
  keep 0, delete - any mailbox the app registration reaches; a key restricted to named domains could do the same,
  and so could a domain administrator over COM, which had been open since 6.2.23 Alpha 1. Only a server
  administrator now adds or changes such an account. The fetchers also sent a bearer token without TLS when the account's security was none: they now
  send one over TLS only ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a relay that refused the OAuth2 login put the server's token in the bounce.** When the relay answered
  `AUTH XOAUTH2` with a refusal - Exchange Online's `535 5.7.3` for a mailbox with SMTP AUTH off, say - the bounce
  to the message's sender quoted the line this server had sent, and that line is the token in base64: the
  server's own, which sends as every mailbox the app registration was granted and, through `FetchOAuth2Hosts`,
  collects them. The SMTP log kept the line too, and the POP3 collector's log its own. The bounce now reads
  `hMailServer sent: AUTH XOAUTH2 <Token removed>` and the logs `AUTH XOAUTH2 ***`, as they have always left
  out a password; open since 6.2.22-pre1.
- **Fixed: an external account's user name or password could add commands to the collection.** Both are sent in
  command lines - POP3 `USER` and `PASS`, IMAP `LOGIN` - and a CR or LF in either ended the command and sent what
  followed as another, to the host and port the account names; the new Other accounts page let any person set
  them. A user name or password with a CR, LF or NUL is now refused by the REST routes and by `FetchAccount.Save`
  over COM, and the fetchers do not send one saved before.
- **Fixed: a person's other account could reach inside the network after it was saved.** Its host was judged
  public once, when it was saved, by the system's resolver; the server later resolved it again with its own DNS
  and connected to whatever came back, on any port from 1 to 65535. A name that answered a public address at
  the save and 127.0.0.1 or 10.x afterwards (DNS rebinding), or one the server's DNS answered differently (split
  horizon), made the server connect inside its own network every fifteen minutes and send a user name and
  password there. Schema 6104 marks an account a person makes on the Other accounts page, or moves to another
  server or port there, as `reader_created`; the fetcher connects such an account only to the public addresses
  the name resolves to at that moment (`HM5909` when there are none), and a person names port 110, 995, 143 or
  993 - both unless `reader_fetch_accounts_private_addresses` is on. Accounts from before 6104, and an
  administrator's, are not marked and behave as they did ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a host added to `FetchOAuth2Hosts` later gave the server's token to accounts others had named.**
  The save refuses anybody without the server's authority a fetch account for a host on the list, but the
  fetchers judged only the host at collection time, so an account a person, a key restricted to named domains
  or a domain administrator had set up for a host the administrator put on the list afterwards logged in with
  the server's own OAuth2 token as whatever user name it gave. Schema 6104 records whose word the server and
  the user name are (`hm_fetchaccounts.faorigin`): an account such a caller makes, or whose server, port or
  user name it changes, is `server_token_withheld`, and the fetchers log it in with its own user name and
  password instead. Accounts from before 6104 are not marked; a domain administrator's saved before this
  release cannot be told apart from a server administrator's
  ([Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: a Web Push endpoint could point the server inside its own network.** With Web Push on (off by
  default), a person could subscribe an https endpoint at any address - a host on the private network, a cloud's
  metadata address - or plain http to any port of the server's own loopback, and the test push answered why the
  connection failed word for word, which mapped the network behind the server one request at a time. A push now
  goes to a public address only; plain http to the loopback is taken only while `WebPushLoopbackHttpForTests`
  (`loopback_http_for_tests` under `/api/v1/settings/push`), a switch for the regression suite that is off by
  default, is on, and then only from a request made on the machine itself that no proxy forwarded - a reverse
  proxy on the same machine that adds no forwarding header makes every person a caller from the machine; a
  failure is shown to the person as "could not be reached" and its reason written to the application log; and a
  push waits ten seconds at most, a test push five.
- **Fixed: one person could stall the webmail and the Deck for everybody.** The REST listener has four workers,
  and a request that waits on another server - a brand logo not yet known, a one-click unsubscribe, an OpenPGP key
  lookup, a test push - held one for up to its deadline, so one account keeping four of them in flight against a
  host that never answers, a made-up subdomain for each logo, left nothing to answer anybody else. An account now
  has one such request in flight at a time and the server two in all; the rest are answered `429` at once. A
  logo lookup or a key directory that took three seconds or more, in the DNS or at the host and whatever it came
  to, is held against its whole organizational domain by the Public Suffix List for an hour, so the next made-up
  subdomain is answered at once without asking it ([BrandLogos.md](hmailserver/docs/BrandLogos.md)).
- **A new installation is secure before anyone configures it.** A database this installation created - never an
  upgraded or attached one - is given a self-signed certificate for the machine (RSA 2048, 825 days), STARTTLS
  offered on 25, 587, 110 and 143, and TLS required for authentication on the Internet IP range, so that a password
  from another machine no longer crosses the network in the clear; My computer is left as it was. The Windows
  installer does it after the database step; on Linux `--create-database` marks the database and the unit's first
  start within a day does it, and the AppImage and the container run `--apply-new-install-defaults` by hand. The
  setup checklist's new Passwords only over TLS item judges every server, new or upgraded
  ([SecureDefaults.md](hmailserver/docs/SecureDefaults.md)).
- **A dedicated low-privilege service account on Windows.** A new installation runs the service as the virtual
  account `NT SERVICE\hMailServer` instead of LocalSystem; an upgrade keeps the account it has, and the installer's
  page, or `/serviceaccount=virtual|localsystem|keep`, offers the other. `hMailServer.exe /GrantServiceAccess`
  grants the account the program folder's Data, Logs, Temp and Database folders and folders an administrator
  approved, and nothing the configuration merely names. `hMailServer.INI` is read-only to the service, which works
  on with the database's values. What only LocalSystem could do is refused with a sentence: the live update's apply
  (an administrator runs the verified installer), the auto-ban's firewall rules and `Utilities.MakeDependent`.
  `ServiceAccountName` and `ServiceAccountPassword` are retired: `/Register` takes the account from
  `--service-account` alone ([ServiceAccount.md](hmailserver/docs/ServiceAccount.md)).
- **What the first security scan found, fixed**: the 7-Zip and the restore above; the in-process testers' 379 bare
  `throw;` statements, which ended the service when a check failed, now fail the test suite; MIME boundaries drawn
  from a per-thread generator seeded from the operating system instead of `rand()` reseeded from the clock; the
  vendored zlib 1.3.2; and VEX statements for the advisories that do not reach the server.
- **An evidence pack for a buyer's security questionnaire**: where mail and personal data are kept, what leaves the
  server, ISO/IEC 27001:2022 Annex A and SOC 2 mapped to what the product provides, and the supply chain, each fact
  with the file it can be checked in and each gap named ([EvidencePack.md](hmailserver/docs/EvidencePack.md)).
- **Fixed: SCRAM-SHA-256 signed in with a password every other path refused.** SMTP `AUTH`, IMAP `AUTHENTICATE`
  and POP3 `AUTH` with `SCRAM-SHA-256` (or `-PLUS`) accepted the account's own password for an account with a
  second factor enrolled - whose own password is otherwise refused in every mail client, app passwords being
  the only credential - and a password past `PasswordPolicyMaximumAgeDays`, and never asked the domain's or the
  account's country policy: SCRAM proves the password without reaching `PasswordValidator` or
  `AccountLogon::Logon`, where those rules are. So a phished password of an account with TOTP could submit mail
  as it and read its mailbox. The second-factor and expiry gaps were in 6.3.3 as well. Each is now a forced
  failure of the exchange, answered as a wrong password is; the country refusal is charged to nothing, as it is
  on the other paths. The `OnClientValidatePassword` script hook is still not consulted for SCRAM, which has no
  password to give it ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md),
  [DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: a sign-in under a domain alias escaped the domain's country policy.** The policy was looked up under
  the domain the client typed, and an alias domain has no policy of its own, so `bob@alias.example` was allowed
  from a refused country and then signed in as `bob@example.com`. The name is now resolved as the sign-in resolves
  it - domain aliases, then the default domain - before the policy is read
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a token, a certificate or a Kerberos ticket signed in past the country policy.** `XOAUTH2`,
  `OAUTHBEARER`, `EXTERNAL` and `GSSAPI` on SMTP, IMAP and POP3 never asked the domain's or the account's
  country policy, which the password paths and the REST API ask, so a stolen bearer token signed in from a
  refused country. They now ask it where they ask the compromise hold (`AccountLogon::RefusedByHold`), and the
  refusal is charged to nothing, as a password's is ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: an SMTP session signed in without a password sent only one message.** At each `MAIL FROM` after the
  first the session's credentials are checked again, and the check validated the password the session kept -
  which only `PLAIN` and `LOGIN` keep. After `SCRAM-SHA-256`, `XOAUTH2`, `OAUTHBEARER`, `EXTERNAL` or `GSSAPI` the
  second message was refused with 550 "Login credentials no longer valid" and the session signed out. Such a
  session now has its account checked again instead: still active in an active domain, not a room, not held for
  review, not refused by its country policy, and for SCRAM not since given a second factor or an expired password.
- **Fixed: the IMAP master user opened a mailbox past the mailbox's country policy.** `AUTHENTICATE PLAIN` with an
  authorisation identity asked the country policy, and the compromise hold, of the master user only, so a master
  user in a domain with no policy signed in as a mailbox from a country the mailbox's domain refuses. The account
  signed into is now asked too, after the password, and the refusal is charged to nothing
  ([CountryBlocking.md](hmailserver/docs/CountryBlocking.md)).
- **Fixed: a sign-in under a domain alias was not the account to SMTP's own controls.** After signing in as
  `bob@alias.example`, the send-stop of the quota policy, the sending quota (counted under a second name), restricted
  delivery, the takeover check, message recall, the address book and the message-size limit looked the account up
  by the name as typed, with the default domain alone, and found nothing - so none of them held. They now use the
  account the sign-in resolved to.
- **Fixed: a domain's maximum message size was lifted by `MAIL FROM:<>`.** A signed-in account was held to its own
  size limit whatever `MAIL FROM` named, but not to its domain's, which was read from the envelope sender's domain;
  naming no sender or another domain's address took the server's instead. The signed-in account's whole answer -
  its own value, else its domain's - now holds as well ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Fixed: the backup-MX recipient callout's ceiling was per recipient domain.** A policy for a pattern such as
  `*.partner.example` gave every subdomain a sender named ten verifications a minute of its own, all to one
  primary, each holding a thread of the SMTP service for up to the timeout, and every address asked about stayed
  in memory. The ceiling is now the policy's and the primary's, at most 8 verifications run at once (2 to one
  server), and the remembered answers are swept and capped at 10,000
  ([RemoteDomains.md](hmailserver/docs/RemoteDomains.md)).
- **Fixed: one connection could put any correspondent on the server's block list.** A message to a spam trap
  blocked its envelope sender server-wide for `SpamTrapBlockDays` (30), whatever that sender was, and the
  envelope is whatever the client types: anybody who knew a trap address - published to be found - could have a
  bank's or a supplier's real mail held or marked for every mailbox. The sender is now blocked only when its
  domain's SPF passes for the connecting address; the address is banned and the hit recorded as before
  ([SpamTraps.md](hmailserver/docs/SpamTraps.md)).
- **Fixed: the send API was not journalled.** A message an application sent through `POST /api/v1/send` reached
  neither of the two points where journal rules are evaluated, so a domain or account under a compliance rule
  had its application mail, and a journalled recipient's copy of it, missing from the archive. It is evaluated
  now, the key's sender counting as the sender when it is an account here. Journaling.md said mail that arrives
  was covered whichever way it arrived; it now says what is not covered in either direction: everything the
  server queues itself - the webmail, a send-later draft, an event script, a rule's forward, copy or reply, an
  automatic reply, a Sieve redirect, an account's forwarding, a delivery report
  ([Journaling.md](hmailserver/docs/Journaling.md)).
- **Fixed: a single sign-on ticket could be redeemed in another person's browser.** Found in the audit before
  this release, in the new OpenID Connect and SAML sign-in. The ticket a finished sign-in carried to the page in
  `/portal#sso=<ticket>` was redeemed by value alone, so anybody who could complete a sign-in of their own could
  send that link to someone else, and the server's own page would sign that person's browser into the sender's
  mailbox - login forgery, past both the `X-Requested-With` check and SAML's `hmsaml` cookie. The ticket is now
  bound to the browser that finished the sign-in by an `hmsso` cookie (HttpOnly, `SameSite=Strict`,
  `Path=/api/v1/session`) set beside it, and `POST /api/v1/session` refuses it without that cookie
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a one-time code asked for on top of a password could be guessed without limit.** Found in the audit
  before this release. Changing the password, making an app password or recovery codes, setting a recovery
  address and registering a passkey ask for the account's password and then its one-time code, and a wrong code
  there was neither counted nor logged, so somebody holding the password and a session (or, for the password
  change, an app password) could try codes at the request rate until one matched and leave with a credential
  that needs no second factor. The administrator's passkey registration had the same gap. A wrong code is now
  logged and charged like a wrong password, and after five in fifteen minutes every code for that account, or
  for the administrator, is refused with 429 for fifteen minutes
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: with a parent-domain relying party, a page on any sibling host could sign somebody in with their
  passkey.** Found in the audit before this release. A passkey's origin was compared with the request's own
  `Host` header, which the client writes, so with `passkey_relying_party` set to `example.com` as the document
  advised, a script on any host under `example.com` could collect a person's assertion and post it here naming
  that host, and be signed in as them with no password and no code. A new setting, `passkey_allowed_origins`,
  names the origins this server's pages are served at; once a relying party is set it is required, and the
  origin of every registration and sign-in must be one it names. A credential made in a frame of another site's
  page is refused too. With neither setting, the default, the check is unchanged
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a switched-off domain still let its people in with a passkey or single sign-on.** Found in the audit
  before this release. A password is refused for an account whose domain is not active, but a passkey, an OpenID
  Connect or SAML sign-in to an existing mailbox, and every browser session already open went on working. All
  three now check the domain, and a session ends at its next request once its domain is switched off
  ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: the forgotten-password route's timing said which mailboxes can be reset.** Found in the audit before
  this release. `POST /api/v1/password-reset` gave every address the same answer, but gave it only after making
  the link and queueing the message when the mailbox had a confirmed recovery address, so the answer came
  measurably later for exactly those mailboxes; and a reverse proxy on the same machine had no per-client limit
  at all. The answer now comes before the address is looked up, the link is made on the async work queue, and
  requests from loopback share a ceiling of thirty in any minute
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a one-time code could be used again for as long as it was in its window.** Found in the audit before
  this release. No place that checks a TOTP code - the COM sign-in (`AuthenticateWithCode`) for an account and for
  the administrator, the administrator's Basic sign-in on the REST API, and the REST routes that ask for a code on
  top of a password - remembered the codes it had accepted, so a code somebody read over a shoulder, out of a
  proxy's log or off a phishing page, with the password beside it, was a whole credential for up to a minute and a
  half after its owner had used it. RFC 6238 says a verifier must not take a code twice. Each owner's last step
  taken is now remembered, in one record every one of those places shares: the same code, or one from an earlier
  step, is refused wherever it comes next. It is not counted as a wrong code - it is no guess, and the one who
  sends it again is most likely its owner - and the wrong codes that are counted are judged and counted under one
  lock, so that requests racing each other cannot all be judged before the fifth is counted
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: a wrong one-time code at the COM sign-in was not counted.** Found in the audit before this release.
  `AuthenticateWithCode` checked an account's code before its password and counted neither, and the
  administrator's wrong code was logged and nothing more, so somebody with the password could guess codes over
  COM at the speed of the calls. The password is now checked first, and a wrong code after a right password
  counts against the same budget the REST API keeps for that owner - five in fifteen minutes, then no code taken
  for fifteen, over COM and REST alike. COM stays outside the per-name lockout and the auto-ban, as its passwords
  are ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy on the same machine, the administrator's one-time code could be guessed at the
  request rate.** Found in the audit before this release; present since the administrator's second factor came
  to the REST API. A wrong code in `X-hMailServer-OTP` on the administrator's Basic sign-in was charged only to
  the auto-ban, which leaves 127.0.0.1 and `::1` alone - and behind a proxy on this machine every request comes
  from there. It now counts against the administrator's budget, and once that is spent the answer is 429 with
  `code` `second_factor_locked` and `Retry-After`; the auto-ban is still charged once for each wrong code
  ([AccountRecovery.md](hmailserver/docs/AccountRecovery.md)).
- **Fixed: behind a reverse proxy that terminates HTTPS, the session cookie was not marked `Secure`.** Found in
  the audit before this release. `hmailsession`, and the sign-in cookies `hmoidc`, `hmsso` (OpenID Connect) and
  `hmpasskey`, were marked `Secure` only when the REST listener itself spoke TLS, so behind the usual proxy in
  front of the plain loopback listener the session cookie of an HTTPS site was one a browser would also send over
  plain HTTP. They now follow the rule SAML's cookies already did: `Secure` over TLS, or when the proxy on this
  machine says `X-Forwarded-Proto: https` ([DevicesAndSessions.md](hmailserver/docs/DevicesAndSessions.md)).
- **Fixed: registering an administrator passkey skipped the one-time code when the administrator's second factor
  could not be opened.** Found in the audit before this release. With `AdministratorTotpSecret` enrolled but its
  secret unopenable on this machine - the key it was protected with lost - every administrator sign-in refuses,
  but `POST /api/v1/passkeys` for the administrator took the password alone from a session already open. It now
  refuses with 409, as the sign-ins do ([PasskeysAndSingleSignOn.md](hmailserver/docs/PasskeysAndSingleSignOn.md)).
- **Fixed: a domain's spam traps were not confined to the domain over REST.** A domain administrator, a role or an
  API key confined to one domain could declare a spam trap in any other domain on the server with `POST
  /api/v1/domains/{domain}/spam-traps` - and a hit on it bans the connecting address and blocks the envelope sender
  for every domain - and read another domain's traps and the senders and addresses that hit them with `GET` on the
  same path and on `.../spam-traps/hits`. The three routes are now scoped to the domain they name, as a trap's
  removal already was, and answer `403` for any other.
- **Fixed: a domain administrator could become the server administrator through an app password.** Where the server
  administrator's own mailbox sits in a domain somebody else administers, that domain's administrator could issue
  an app password on it (`POST /api/v1/accounts/{address}/app-passwords`, or COM's `AppPasswords.Add`), was handed
  the secret, and could then log on to COM with it - `Application.Authenticate` accepted an account's app password
  and gave the logon the account's whole role, past its second factor. COM no longer accepts an app password as a
  logon at all (it still opens the mailbox over IMAP, POP3 and SMTP), and a credential may no longer issue or take
  back an app password, share a folder, or write, flag, copy or delete mail, or revoke a device, of an account
  whose authority it does not take in, over REST or COM ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could delete or rename the server administrator's mailbox in its domain.** `DELETE
  /api/v1/accounts/{address}` and `POST /api/v1/accounts/{address}/move` asked only whether the account's domain
  was the credential's, so the server administrator's own mailbox could be deleted with its mail, or renamed so
  that its logon name no longer existed. Both now refuse, with `403`, an account whose authority the credential
  does not take in, as COM's `Account.MoveTo` always did; COM's `Account.Delete`, `Accounts.Delete` and
  `Accounts.DeleteByDBID` refuse it too ([Roles.md](hmailserver/docs/Roles.md),
  [MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- **Fixed: the REST listener read request headers out of the body.** A header the request's head did not carry was
  looked for in the body as well, so a cross-site form posting `text/plain` to `/api/v1/session` could carry an
  `Authorization` line of an account the attacker held and sign the victim's browser in to it (login CSRF), and an
  `X-Requested-With` line in a body satisfied the guards that ask for that header. Every header is now read from
  the head alone.
- **Fixed: a credential confined to one domain could set the webmail's branding for every domain.** `PUT
  /api/v1/portal/branding` and `PUT /api/v1/portal/files` name their domain in the body, where the authorisation
  did not look, so an API key or a role holding `manage_settings` and confined to one domain could rewrite the
  server's sign-in announcement and logo, or another domain's, and the file-link policy of either. A confined
  credential now sets only its own domains' and is refused the server's and any other's with `403`, and a pass
  over the scheduled sends (`POST /api/v1/scheduled/run`) is refused to it as server-wide
  ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a domain administrator could repoint the server administrator's fetch account.** The routes under
  `/api/v1/accounts/{address}/` for an account's rules, fetch accounts, OpenPGP keys, directory entry, booking
  policy, address-book shares, push devices, recovery address and codes and review state asked only whether the
  account's domain was the credential's. Where the server administrator's own mailbox sits in a domain somebody
  else administers, that domain's administrator could point its fetch account at a host of its own - the server
  then signed in there with the kept password - set rules on the mailbox, publish a key under its address, clear
  its recovery or exempt it from the compromised-account detection; a support session into the mailbox and a
  migration job copying mail into it asked the same and no more. Every route that changes an account named in its
  path now asks one rule, in one place, and refuses with `403` an account whose authority the credential does not
  take in; `build/check-role-choke-point.py` fails a new such route that is not in it. A migration, or a job of
  it, is started, paused, resumed, retried, cancelled or deleted only by a credential that takes in every account
  its jobs copy into ([Roles.md](hmailserver/docs/Roles.md), [Migration.md](hmailserver/docs/Migration.md)).
- **Fixed: over COM, a domain administrator could change what is in the server administrator's mailbox in its
  domain, and point a rule or a fetch account at another domain's account.** COM's `FetchAccount`, `Rule` (with its
  actions and criteria), `IMAPFolder`, `IMAPFolderPermission` and `Message` writes, and `Account.DeleteMessages`,
  `RevokeDevice`, `ClearRecoveryAddress`, `ClearRecoveryCodes` and `RemovePushSubscriptions`, asked nothing of the
  account they were in - only how the object had been reached - so a domain administrator could add a fetch account
  or a rule to the server administrator's mailbox, share or delete its folders and mail, or clear its recovery; and
  a rule, a rule's action or a fetch account reached in its own domain could be given another account's id, in any
  domain, and saved there. Each now asks where it writes whether the credential reaches the account (the server
  administrator, an administrator of its domain, or the account itself) and takes in its authority, and refuses
  otherwise; an event script's objects are left as they were ([Roles.md](hmailserver/docs/Roles.md)).
- **Fixed: a key or a role confined to a domain could route the server administrator's mail in that domain to a URL
  of its own.** An inbound webhook route was judged by its domain alone, and a route takes an address's mail instead
  of its mailbox, so a credential holding `manage_inbound_webhooks` and confined to a domain could route the server
  administrator's address there - or the whole domain - and read its mail, password resets included. A route is now
  made, changed or removed only by a credential that takes in the account at its address (or the one its plus tag
  reaches), and a domain's route only by one that takes in every account of the domain with an admin level or a
  role; the refusal is `403` ([InboundWebhooks.md](hmailserver/docs/InboundWebhooks.md)).
- **Fixed: a domain administrator could not clear its own users' recovery address, recovery codes or push devices
  over REST.** `DELETE /api/v1/accounts/{address}/recovery`, `.../recovery/codes` and `.../push` (and `GET .../push`)
  read the whole address as a domain name, which no credential confined to a domain lists, so they answered `403`
  to every domain administrator and every key issued for named domains. They are now scoped to the address's
  domain, as the other account routes are.
- **Fixed: a large download over the REST API or the web services was cut off, and held whole in memory while it
  lasted.** The deadline that covers reading a request covered writing its response too, so a file link, an export
  or a message source the reader could not take in within the request deadline (30 seconds on the REST listener,
  15 on the web services) ended part way, every time - a 100 MB file link below about 27 Mbit/s - and the
  connection's five-minute limit cut off whatever outlived it. Meanwhile each download held the whole body three
  or four times over, so 64 slow readers of a 500 MB file link, which needs no sign-in, could hold some 32 GB of the
  mail server's memory. A response is now written in pieces and the deadline is progress: a reader that stops
  reading is closed after the request deadline. A file link, the export archive (`/export.zip`) and an eDiscovery
  export are sent from their file on disk 64 KB at a time, never read into memory whole. A large download - a
  file, or a body of 4 MB or more - may outlive the connection's limit for as long as its size takes at 128 KB a
  second, and at most sixteen are written at once; one more is answered `503` with `Retry-After: 30`
  ([PersonalData.md](hmailserver/docs/PersonalData.md), [Discovery.md](hmailserver/docs/Discovery.md)).
- **Fixed: a request head that never ended could hold 16 MB per connection.** The listener read a head into the
  buffer sized for the largest body a route takes and applied the 64 KB cap on a head only once its blank line
  arrived, so each connection could be made to hold 16 MB until the request deadline closed it. A head is now
  refused with `413` as soon as it passes the cap.
- Fixed: BATV tags, SRS addresses, the fabricated SCRAM salt of an unknown account and the Web Push endpoint's
  hash were made over a conversion that, on Windows, gave zero bytes for everything after the first character
  above U+00FF and, on Linux, `?` for each such character - the conversion the audit trail's chain was fixed of
  (under Administration). So two internationalised mailboxes of one length shared a BATV tag and an SRS hash, and
  a bounce to one was taken, or reversed, as a bounce to the other; a database password of Cyrillic letters added
  only its length to the SCRAM key; and two push endpoints of one length shared the hash the unique index is on.
  Each is now made over UTF-8. A String that holds UTF-8 widened one byte to a character - what the SMTP and IMAP
  command lines give, and so what BATV and SRS are handed - is hashed as those bytes, which the old conversion
  hashed too; so for such a mailbox, as for ASCII, a tag or an SRS address handed out before the upgrade still
  verifies, and the salt an unknown account is given does not change. `ClassTester` holds each to the old
  conversion over ASCII and over widened UTF-8, and to a difference over Cyrillic, and `Batv.cs` accepts a tag
  computed the old way. A push endpoint outside printable ASCII - which no browser gives - is refused.
- Fixed: a password of Cyrillic, Greek or CJK letters could be opened by any other password of its length. Every
  password hash (MD5, salted SHA-256, PBKDF2, Argon2id, scrypt, and the pepper) was made over the same
  conversion - NUL bytes on Windows from the first character above U+00FF on, `?` for each on Linux - so SASL
  PLAIN, whose fields are UTF-8, took `ключик-вход` for `пароль-тест`, and on Windows a REST API Basic credential
  of NUL bytes took either; the administrator password was hashed the same way. Hashes are now made over UTF-8,
  which leaves every ASCII password's hash as it was and lets such a password serve SCRAM-SHA-256. A password that
  arrives as its UTF-8 widened one byte to a character (IMAP `LOGIN`, REST Basic) is hashed as those bytes, as it
  was, so it and the same password decoded are one; a sign-in also tries a Latin-1 password's characters as bytes
  (for a client that sends UTF-8, those of the text it decodes to), which opens a hash made of them before, and
  that sign-in makes the hash again over UTF-8 (as it does one made under a pepper outside ASCII, under
  the pepper's UTF-8), after which it serves SCRAM-SHA-256 too. A password holding U+0000 is refused. COM's
  `Utilities.MD5` and `Utilities.SHA256` are the same code, so for text outside ASCII they now give the digest of
  its UTF-8. A hash the old conversion made of a password above U+00FF is refused and logged (upgrade item 24);
  on Linux such a hash still opens to the password with `?` for those characters until it is set again, and on
  both platforms SCRAM-SHA-256 still opens a PBKDF2 one to the bytes it was made over
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md), *A password outside Latin-1*).
- Fixed: the crash-dump helper, `hMailServer.Minidump.exe`, loaded `dbghelp.dll` by name, so a copy put in `Bin`
  would have run under the service's account at the next crash. It loads System32's, and the service starts
  the helper by its full path.
- Fixed: the Control Panel restarted the service after a settings change by starting `cmd.exe` elevated by name,
  and Windows looks for a bare name in the working directory first - wherever a file dialog last left it - so
  a `cmd.exe` put there would have run behind the elevation prompt. It runs System32's `cmd.exe` and
  `net.exe` by their full paths, and `DBUpdater.exe`, run as an administrator, opens an upgrade script in
  System32's Notepad by its full path, where it named `notepad.exe` alone.
- Fixed: the MySQL client's own DLLs were looked for in the current directory and along `PATH` as well; they
  are looked for beside `libmysql.dll` and in System32 only. The MariaDB client the x64 installer ships needs
  nothing else; a client copied into `Bin` by hand needs its DLLs beside it.

### Encryption

- **The server publishes its users' OpenPGP keys** (schema 6050) through Web Key Directory; publishing is off
  by default, and a secret key is refused at every entry point.
- **OpenPGP in the webmail** (schema 6080): end-to-end encryption in the browser, with the keys under a
  passphrase this server never sees; a correspondent's key remembered at first sight and a changed key shown
  as a warning (schema 6081); and a password-protected message for a correspondent with no key (schema 6082)
  ([OpenPGP.md](hmailserver/docs/OpenPGP.md)).
- **Zero-access mailboxes** (schema 6083): opt-in per account, delivered mail is encrypted to the account's own
  key once the filters have run, and the administrator cannot read, search or recover it
  ([ZeroAccess.md](hmailserver/docs/ZeroAccess.md)). A message that arrives already encrypted is stored as
  it arrived; whether it is, is read from its body and not its `Content-Type` - fixed before release, when a
  `multipart/encrypted` label over plain text, or an S/MIME v2 opaque-signed message with no `smime-type`,
  was stored in the clear; and the whole message has to be under the key, not merely hold some ciphertext -
  fixed before release, when real PGP/MIME ciphertext with a readable part beside it was stored as it arrived,
  that part in the clear. Gateway encryption to partner domains makes the same decision the same way, and
  encrypts such a message rather than sending the readable part as it was.
- **Gateway encryption to partner domains** (schema 6094): a remote-domain policy encrypts mail to a partner's
  OpenPGP key or S/MIME certificate at this server, and says whether to refuse or send unencrypted when it
  cannot.
- **An organisation CA for S/MIME** (schema 6095): a domain's own authority certifies keys its people generate
  in the browser. It never makes a person's key, and the private key stays wrapped where this server cannot
  open it, so nothing here can recover mail encrypted to a lost key
  ([SmimeAuthority.md](hmailserver/docs/SmimeAuthority.md)).
- **The webmail's S/MIME made honest**: signing out takes the private keys with it, an administrator's password
  change re-wraps them, and a message that was to be sealed is refused as a draft rather than stored in the
  clear.
- **Mailbox encryption at rest** (schema 6099): switched on per account, after the domain's recovery key has been
  made, every message stored in the account's folders from then on is encrypted to an X25519 key of the account's
  own, with AES-256-GCM, as the message store's last stage. Mail that arrives while nobody is signed in is
  encrypted all the same; reading it needs the private half, which the account's own password opens at sign-in and
  which closes `MailboxKeyIdleMinutes` (15) after the last use. App passwords are refused for such a mailbox, an
  administrator's password reset is refused while the key is closed and goes through a recovery with the domain's
  passphrase, and it cannot be combined with zero access or a TOTP second factor. It is on REST and the Control
  Deck, with no webmail card and no Control Panel page, and `--encrypt-store` and `--decrypt-store` rewrite what
  was stored before. Off on every account ([EncryptionAtRest.md](hmailserver/docs/EncryptionAtRest.md)).
- **Secrets at rest** (schema 6103): an account's TOTP second factor and the `[Settings]` secrets (`SRSSecret`,
  `BATVSecret`, `OAuth2HmacSecret` and the rest) are sealed with AES-256-GCM under a stored-secrets key the
  installation's servers share, kept in each server's `hMailServer.ini` under DPAPI or the Linux key file, never in
  the database or a backup; the database records which key it is. The Windows installer makes the key on a new
  installation, and the container's `--create-database` on Linux; after an upgrade nothing is sealed until an
  administrator runs `--create-stored-secrets-key`, which the setup checklist offers. `--export-stored-secrets-key`
  and `--import-stored-secrets-key` move it to another server, the checklist stays open until it has been exported,
  and `--forget-sealed-secrets` with a lost key's id clears only what that key sealed.
  `ProtectStoredSecretsWithDPAPI=0` keeps everything as written. The step widens `accounttotpsecret` to 1024
  characters. The installer now writes the administrator password as PBKDF2 itself
  ([SecretsAtRest.md](hmailserver/docs/SecretsAtRest.md)).
- **A certificate for an IP address is accepted on Linux**: an HTTPS endpoint given by its address - an object
  store, a webhook, an identity provider, the update feed - was refused when its certificate named the address as
  an iPAddress entry, which is where RFC 5280 puts it; and no server name is sent for an address.

### Administration

- **One value, three levels** (schema 6088): 27 settings may be set for the server, a domain or an account,
  with every interface showing what is in effect and where it came from
  ([SettingLevels.md](hmailserver/docs/SettingLevels.md)).
- **Profiles**: office, gateway, relay and sender, each a documented set of settings applied with a diff first
  and a way back ([Profiles.md](hmailserver/docs/Profiles.md)); **what this server has changed from stock**
  ([ChangedFromStock.md](hmailserver/docs/ChangedFromStock.md)).
- **Two command-line clients over REST**, `hmctl` (installed by the Linux packages) and the `HmailServer`
  PowerShell module, with the whole configuration as one document to export, diff and apply, and accounts
  imported from and exported to a spreadsheet ([CommandLine.md](hmailserver/docs/CommandLine.md)).
- **The audit trail** (schema 6043) as a hash chain that can be verified, a setting's change put back from its
  row, and a Control Panel page; its first gaps - misspelt table names, the settings store, the access lists -
  closed. **Alerts** now cover eleven conditions, by mail or signed webhook.
- Fixed: the audit trail's chain did not cover a character above U+00FF. The fields were hashed through a
  conversion that, on Windows, hashed everything after the first such character as zero bytes and, on Linux,
  each such character as `?`, so a row naming a Cyrillic account or an internationalised address could be
  edited after that character (`deleted` to `created`, say) and `verify` still answered intact. The fields
  are now hashed as UTF-8 and the hash written in upper case; rows written before, in lower case, are checked
  the old way ([AlertsAndAuditTrail.md](hmailserver/docs/AlertsAndAuditTrail.md), *Verifying the chain*).
  `GET /api/v1/audit` listed such a row through the same conversion, so its actor, object and detail came back
  as zero bytes or question marks, and its `actor`, `object_type` and `action` filters matched nothing outside
  ASCII; the listing is now UTF-8 and the filters are read as UTF-8.
- Fixed: the alerts carried text outside ASCII as something else. The REST routes widened the JSON body's UTF-8
  one byte at a time, so a rule's condition, address, webhook and secret, and a test alert's condition and
  summary, were stored as other text than was sent, and a condition was counted in bytes against its limit of 64
  characters; and
  `GET /api/v1/alerts/rules` and `/events`, the webhook's body and the mail wrote text through the conversion
  above, so an alert the server raised about a name outside Latin-1 arrived with zero bytes or question marks.
  Every field is now read and written as UTF-8; the mail's Subject is RFC 2047 encoded words when it is not
  ASCII; the condition in `X-hMailServer-Event` is percent-encoded outside printable ASCII, so no byte of it can
  end the header; and the webhook is signed with the secret's UTF-8, which for an ASCII secret and body is the
  signature it always was (`ClassTester` holds it to a value computed apart); a secret stored before this fix,
  as the bytes it was sent as, signs with those bytes, so no receiver's check changes. Text that arrived that
  way - an address from the SMTP command line, a rule written before - is written as the UTF-8 it stands for.
- **A setup checklist the server judges**, a sample domain that can be thrown away, **help beside every page**
  of the Control Panel, the Deck and the webmail from the documents built into the binary, and first-run tours
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md), [SampleDomain.md](hmailserver/docs/SampleDomain.md),
  [Help.md](hmailserver/docs/Help.md)).
- **The Control Panel on one design system**: a command bar, a sidebar that collapses to a rail and dialogs
  sized to their content ([ControlPanelDesign.md](hmailserver/docs/ControlPanelDesign.md)); 65 pages, ten more
  than at 6.3.3.
- **The Control Deck** in twenty languages besides English, redrawn on the webmail's design and usable without a pointer, with
  one connection's log followed live ([FollowConnection.md](hmailserver/docs/FollowConnection.md)). The census in
  [DeckParity.md](hmailserver/docs/DeckParity.md) counts 458 of the 466 properties the desktop Control Panel
  writes as writable over REST and reachable from a Deck view; the eight it lists as missing are an account's
  directory fields.
- **A person's data as one archive**, and an erasure that reaches the rest of it
  ([PersonalData.md](hmailserver/docs/PersonalData.md)); **mailbox rename and move** as one operation (schema 6056)
  ([MailboxMove.md](hmailserver/docs/MailboxMove.md)).
- Fixed, in the person's export new in this release: an app password could download the whole account at
  `GET /api/v1/me/export.zip`, and the archive's `account.xml` carried the password's hash, the authenticator's
  seed in the clear, the app passwords' hashes and the fetch accounts' passwords and OAuth tokens, sealed only
  with a key built into every copy of the server. The download is now refused (403) to an app password and to a
  webmail session started with one, and `account.xml` - in every form of the export - leaves out the password,
  the seed, the hashes of the app passwords, recovery codes, password history and file passwords, the fetch
  accounts' secrets, a browser's push secret and the wrapped mailbox key of an account encrypted at rest
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed, in the same export: its `logs.txt` took every log line in which the address occurred inside another,
  so that `ann@example.com`'s own download carried the sessions, peers and recipients of `joann@example.com` and
  `ann@example.com.au`. The address is now matched whole.
- Fixed, in the same export: each folder was built as one mbox in memory, deflated beside a copy of itself, and
  the 500 MB download ceiling was checked only once the whole archive was written, with nothing limiting how
  many were built at once - so any account, four requests at a time, could hold every HTTP worker (the webmail's
  and the whole API's) for minutes and push the service toward running out of memory. A folder past 256 MB is
  now written in parts, a mailbox whose mail alone is past 2 GB is refused (413) before anything is built, and a
  download is refused while an archive of the same account is being built (409) or two are on the server (429).
- Fixed, in the erasure of an address from the logs new in this release: every occurrence of the address inside
  another was rewritten too, so that erasing `ann@example.com` turned `joann@example.com` into `jo[erased]` and
  counted it as removed, and each log was truncated and written in place, so that a crash or a full disk part way
  lost the rest of it. The address is matched whole - still where a line quotes it (`'ann@example.com'`) or
  writes it after `=` or `/`, though not as the last part of an SRS address, which is another person's - and each
  log is written beside itself and renamed over it.
- Fixed: on PostgreSQL an erasure compared the message trace, the greylisting triplets, list memberships and
  aliases with the address exactly as given, and PostgreSQL compares text by its bytes, so a row written in another
  case (`Ann@Example.com`) survived an erasure of `ann@example.com` that reported success. Every store the
  erasure deletes rows from now compares both sides lower-cased.
- **eDiscovery** (schema 6097): a compliance officer opens a case, searches every mailbox, the deleted items and
  the archive by person, date, words and attachment, puts the mailboxes, domains or archived copies it is about on
  a legal hold, and exports what was found as EML and mbox with a manifest whose SHA-256s prove what was handed
  over. A hold stops the account's and its domain's deletion, a mailbox move to another domain, a purge of its
  deleted items, the retention sweep and a full restore, and keeps what its owner deletes among the deleted items
  until it is released. Every search, hold, export and download is on the audit trail. Two permissions,
  `view_discovery` (which the auditor role holds) and `manage_discovery`; the routes are `/api/v1/discovery/cases`,
  with a view on the Control Deck and a read-only page in the Control Panel
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- Fixed, in the eDiscovery export new in this release: the .zip counted its entries in the 16 bits of the
  classic end record alone, so an export of more than 65,535 entries - a search may keep 100,000 hits, each
  written as an .eml - ended with a count modulo 65,536, and a reader that trusts it, .NET's `ZipArchive` among
  them, refused the evidence package or listed part of it. From 65,535 entries on the archive ends with the ZIP64
  end record and its locator, which count them in 64 bits; the person's export is written the same way
  ([Discovery.md](hmailserver/docs/Discovery.md)).
- **An erasure reaches the stores beside the mailbox**: messages held for review, messages that expire,
  password-protected messages, the recipients an account remembers writing to, the out-of-office memory and the
  correspondents' S/MIME certificates and OpenPGP keys. Every comma-joined list is matched whole, so that erasing
  `ann@` never touches `joann@`, and the archive index's own match is fixed the same way. A legal hold outranks the
  erasure, and `POST /api/v1/addresses/erase` answers `held` beside `removed`
  ([PersonalData.md](hmailserver/docs/PersonalData.md)).
- Fixed: a domain could be given the single-label name of a directory the server keeps its own files in under
  the data directory - `protected`, `confidential`, `recoverable`, `discovery` and the rest - over COM and REST
  (the Control Panel asked for a dot), and then shared that directory with the store. A backup left such a
  domain's mail out and a restore did not put it back; on Windows the recoverable, confidential and protected
  stores' clean-up could remove a mailbox directory named by a number; and renaming the domain carried the whole
  directory, the store's files with it, into the new name and deleted it where the store looks. Such a name is
  now refused for a new domain and for a rename; an existing one is reported at every start (`HM7597`) and on the
  checklist, and renaming it moves only its accounts' mailboxes
  ([SetupChecklist.md](hmailserver/docs/SetupChecklist.md)). The operator's `GET /api/v1/accounts/{address}/export.zip`
  in the OpenAPI document now says, as the download does, that the credentials are left out of `account.xml`, and
  lists its 403, 409 and 429 answers.
- Fixed: renaming a domain failed half-way when any of its accounts had a Sieve script, a vacation reply's memory
  or a duplicate check on disk. The domain's Sieve tree was moved first, and each account's rename then refused
  to move its own Sieve directory onto the one that move had just made - after the mailboxes had moved and before
  the row took the new name, so the domain's mail was where its name did not look, and a second rename was refused
  because the new directory existed. An account's rename in the cascade now moves nothing that is not left under
  the old name.
- Fixed: 72 COM classes answered a refusal as a bare HRESULT; each now says why. The message trace's search
  answered nothing on SQL Server Compact (one parameter written twice and bound once).

### Migration and import

- **A migration that survives being interrupted** (schema 6090): mailboxes copied from another IMAP server,
  Google Workspace or Microsoft 365 as jobs, one per mailbox, throttled, resumed without copying anything twice,
  with contacts and calendars beside the mail ([Migration.md](hmailserver/docs/Migration.md)).
- **Other accounts in the webmail** (schema 6065): POP3 and IMAP accounts fetched from the page, and an import
  from Gmail or Outlook.com with a sign-in (OAuth with PKCE).
- **Fixed: one person could switch every import sign-in off.** The table of sign-ins waiting to finish refused new
  ones once it held a thousand, and each lived ten minutes, so one account starting an import a thousand times in
  under a minute kept every other person's import and every migration job's Google or Microsoft sign-in refused
  for as long as it went on. An account or a job now keeps three at most, a fourth dropping its own oldest, and a
  full table drops its oldest instead of refusing ([Migration.md](hmailserver/docs/Migration.md)).
- **PST import**: the server reads Outlook data files itself, with a reader of this project's own written from
  Microsoft's published specifications, as a migration of kind `pst` - one job per file into one account, with the
  migration row's throttle, resume marks, report and Deck view. Mail, folders and their flags and categories come
  in, and contacts, calendar items and tasks go into the account's address books and calendars as vCard and
  iCalendar, notes and journal entries as messages. ANSI files are read, and an OST where it is a PST in all but
  its name; the 4K-page layout, Windows Information Protection and an OST caching an Exchange mailbox are refused
  with a sentence saying what to do. The reader runs in a process of its own, killed on a pause, a cancel or a
  stop, so a file that crashes it costs one message, not the service. An administrator copies the file into
  `<PstImportDirectory>/<domain>/` - empty as shipped, so no PST job runs until an administrator names a
  directory - and the job is started over REST (`POST /api/v1/migrations`), `hmctl` or the Deck
  ([Migration.md](hmailserver/docs/Migration.md)).

### Lists and bounces

- **Bounces read and a suppression list** (schema 6049): delivery reports classified and counted per address,
  hard-bounced addresses suppressed; off by default ([BounceSuppression.md](hmailserver/docs/BounceSuppression.md)).
- **Complaints read** (schema 6055): the providers' feedback-loop reports recorded and fed to the suppression
  list; off by default ([Complaints.md](hmailserver/docs/Complaints.md)).
- **A distribution list that defines itself** (schema 6074): its members given as a query over the server's
  accounts ([DistributionLists.md](hmailserver/docs/DistributionLists.md)).
- **A list manager and bounce handling for lists** (schema 6092): subscribe and unsubscribe by mail or page with
  confirmation, one-click unsubscribe, an archive, a digest, VERP and a footer, and members paused after hard or
  repeated soft bounces; nothing changes for a list until it is switched on
  ([MailingLists.md](hmailserver/docs/MailingLists.md)).

### Operations

- **The update feed is off GitHub.** The live update reads `https://updates.progressiverobot.com`, which GitLab
  Pages serves from the project's releases, in place of GitHub's Releases API, which has answered 404 since 18
  September 2026. It trusts two signers: the GitHub workflow that signed every release up to 6.3.3, for a
  certificate issued before the suspension and a release no later than 6.3.3, and this project's GitLab CI pipeline
  for a `v*` tag, at the project's path and at the group's it is to move to (below), which is to sign the releases
  from 6.3.5 and has signed none yet. 6.3.4 was built and
  Authenticode-signed by hand, before that job existed, and carries no Sigstore bundle: every server, of any
  version, installs it by hand, and a live update offered it says it cannot be verified and downloads nothing. A
  file signed for one release tag is refused as any other release; on Windows the installer's own version resource
  must name the release it is installed as; and a version with a suffix is a pre-release whatever the feed says. On
  Windows the `Updates` folder admits changes from SYSTEM, the Administrators and the service alone, and the update
  helper runs the installer, and the rollback image, only with the SHA-256 the server read when it checked them,
  through a handle it holds until they have finished. **A server from 6.2.28 to 6.3.3 still looks at GitHub.**
  Under `[Settings]` in its `hMailServer.ini`, add
  `UpdateFeedUrl=https://updates.progressiverobot.com/releases.json`, then restart the service, and its update
  check works again. Its live update can then install the re-hosted releases up to 6.3.3. The first release signed
  on GitLab, 6.3.5, will be refused by that server's live update, which trusts only GitHub's signer; installing
  6.3.4 by hand first avoids it, since 6.3.4 trusts both. Until then keep `UpdateAutoDownload` off, or such a
  server downloads a GitLab-signed installer at every check and refuses it
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **Stable download links.** The feed's site answers `https://updates.progressiverobot.com/download/<name>`, where
  `<name>` is a file's name without its version - `hMailServer-x64.exe`, `hmailserver_amd64.deb`,
  `hmailserver.x86_64.rpm` and the rest, each with its `.cosign.bundle` beside it - with a redirect to that file of
  the newest stable release, and `/download/` with one to that release's page. GitLab's own
  `/-/releases/permalink/latest` follows the release published last, a pre-release included; these links never do,
  and a name the stable release does not carry answers 404, never another release's file. 6.3.4 carries the x64
  installer, the amd64 `.deb` and the x86-64 `.rpm`, and no bundle
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md#the-download-links)).
- **The live update already trusts where the project is going.** The project is to move from the personal
  namespace `Progressiverobot/hmailserver` to the group [`hmailserver`](https://gitlab.com/hmailserver), which the
  owner reserved on 26 September 2026, and a move changes the path every signing certificate names. A server trusts
  only the signers compiled into it, so 6.3.4 carries a third: the same pipeline at
  `https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v<version>`, issuer `https://gitlab.com`,
  repository `https://gitlab.com/hmailserver/hmailserver`, with the project's id, `86730559`, which a transfer keeps,
  required in the certificate whatever `UpdateSigningIdentity`, `UpdateSigningIssuer` and `UpdateSourceRepository`
  say (`-` included) - so nobody who registers that path, now or later, can sign as this project, and a release
  signed after the move installs by the live update of every server from 6.3.4 on (the project moves only once a
  release signed before it has shown that Fulcio records the id). The id stays optional for the present path. A
  server that still has the three GitLab values `UpdateFeed.md` gave 6.2.28 to 6.3.3 set trusts the present path
  alone, and refuses what is signed after the move until they are cleared; a server from 6.2.28 to 6.3.3 checks no
  id, is never to be pointed at the group's path with them, and is upgraded by hand after the move
  ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **From 6.3.5, a release is made by GitLab CI**, from a protected `v*` tag. Only a Maintainer may create one, and
  since 26 September 2026 the project's only member with that role or above is its owner, whose tags these are
  ([GOVERNANCE.md](GOVERNANCE.md)). The release job refuses a tag that is not annotated, not on `master`, or not
  signed by the one key `master`'s `.gitlab/allowed_signers` lists, read from `master` and never from the tagged
  tree, and a file name the package registry already holds with other bytes; and it refuses while the security
  scans of the tagged commit have a finding the policy counts that no baseline holds and `master`'s
  `.gitlab/security-exceptions.yml` does not accept (see Build and tests). The pipeline builds the installer on
  the project's Windows runner after the whole suite has passed there; once the owner has started the `release`
  job, it Authenticode-signs the installer through Azure Artifact Signing with a federated credential made for
  that one tag, makes the signed installer's own SBOM, signs every file with Sigstore as the pipeline, checks each
  signature with the server's own `--verify-update`, and, once the installer smoke test has passed and the owner
  has started the last job,
  publishes the files to the package registry and a GitLab release, in the milestone of its version where the
  project has one, with this file's section as its notes, and refreshes the update feed. The job's
  checks run as the tagged commit has them, so they stop a mistake; what stops a hostile tag is that nobody else
  may create one. The container image, `registry.gitlab.com/progressiverobot/hmailserver`, is pushed, signed with
  cosign and scanned by the same pipeline once the project has moved to the group and the owner has switched it
  on, since GitLab.com cannot move a project whose registry holds an image; signed apt and dnf repositories on the
  feed's site are made from the stable releases' packages once the owner's signing key exists - an apt suite per
  distribution release, 6.3.4's `.deb` in `ubuntu26.04`'s, and dnf repositories only from the first release that
  carries `.rpm` files built on each distribution, since the one `.rpm` 6.3.4 carries is built on Ubuntu and goes
  in none ([PackageManagers.md](hmailserver/docs/PackageManagers.md#apt-and-dnf)). None of this has
  run: 6.3.4 was made by hand ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md),
  [RELEASE.md](RELEASE.md), [PackageManagers.md](hmailserver/docs/PackageManagers.md)).
- **Upgrades without a booked window**: a drain closes the listeners before it waits, the live update drains
  first, and with `UpgradeOnStart=1` in `[Database]` (a new bootstrap key, 0 by default) the service applies
  the schema steps itself as it starts ([Upgrading.md](hmailserver/docs/Upgrading.md)). A rolled-back update's
  outcome is reported as soon as the service is back.
- Fixed: on MySQL and MariaDB, two nodes applying the schema steps at once - both with `UpgradeOnStart=1`, or one
  and `--upgrade-database` - were not serialised: the version-row lock the second waited on was freed by the
  step's first DDL statement, so it ran the same step beside the first and stopped with a table or a column
  already there. Each step now holds a named lock (`GET_LOCK`) until it is over. A server that refuses
  `GET_LOCK` (MariaDB Galera) still upgrades, with the row lock alone, and says so for each step.
- **Backups**: encrypted with AES-256, the passphrase never inside the archive (schema 6059); one mailbox or one
  message restored from an archive ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a backup with **Back up messages** copied `<data directory>\Confidential` and `<data directory>\Protected`,
  so the whole of every live message that expires - attachments and all, in the clear even for a mailbox encrypted
  at rest - and the ciphertext of every password-protected message outlived its expiry and withdrawal in each
  archive, and a full restore deleted and replaced both folders. Both are now left out of the backup and left in
  place by a restore, as the recoverable store and the eDiscovery files are. Archives taken by earlier 6.3.4 builds
  still hold those copies ([Backup.md](hmailserver/docs/Backup.md)).
- Fixed: a restore whose backup index was cut short or altered - ending inside a tag, a comment or a quoted
  value, or holding an entity reference without its `;` - read past the end of the index. Such an index is
  refused as malformed - a comment left open after its closing tag is only ignored - and a reference without
  its `;` is kept as written.
- **Monitoring**: syslog (RFC 5424, or the journal under systemd; off) ([Syslog.md](hmailserver/docs/Syslog.md));
  per-domain reports ([Reports.md](hmailserver/docs/Reports.md)) with spam, virus and storage figures per domain
  (schema 6062); the DMARC and TLS-RPT reports a domain receives, read and charted (schema 6070)
  ([DmarcReports.md](hmailserver/docs/DmarcReports.md)); outbound webhooks for five mail events (schema 6051)
  ([Webhooks.md](hmailserver/docs/Webhooks.md)); a daily check of the server's own addresses against block lists
  (schema 6057), on in a new installation and off after an upgrade
  ([ReputationWatch.md](hmailserver/docs/ReputationWatch.md)).
- **Fixed: on Linux, a syslog collector that went quiet held the service.** Over TCP or TLS the sink's connection,
  handshake and writes had only the socket's own timeouts, which Asio's synchronous calls do not honour on Linux,
  so TLS pointed at a plain-TCP port, or a collector that accepted and stopped reading, stopped the sink for good
  and left a reinitialise or a service stop waiting on it until systemd killed the service. Each step now has ten
  seconds and is given up on after that ([Syslog.md](hmailserver/docs/Syslog.md)).
- **Fixed: on Linux, an OpenTelemetry collector that went quiet held its exporter.** The OTLP exporters' three-second
  send and receive deadlines were passed in the Windows form, which Linux refuses, so a collector that accepted the
  connection and never answered held the exporter, and the stop that waits for it, for good. Linux is given the
  deadlines in its own form. This was so in every release with Linux packages, from 6.3.0.
- Fixed: the reports (`GET /api/v1/reports`) keyed and wrote every address and domain through the conversion the
  audit trail's chain was fixed of, so two senders or recipients at a local domain whose names differ after a
  character above U+00FF were counted as one row, named as zero bytes on Windows and question marks on Linux, in
  the JSON and in the CSV. Names are now keyed and written as UTF-8, domains lower-cased in A to Z as before, and the
  `domain` parameter is read as UTF-8. A trace row that holds an address as the SMTP command line gives it, its
  UTF-8 widened one byte to a character, is written as the UTF-8 it stands for, as it was, and counted with the
  same address decoded.
- **Linux**: the live update applies on Linux through a root helper outside the server's unit; a Helm chart and a
  Compose file beside the container image; `hmailserver --set-setting`, `--sample-domain` and
  `--apply-environment`.
- **Windows packages**: `winget` and Chocolatey manifests rendered from the installer a release actually
  published ([PackageManagers.md](hmailserver/docs/PackageManagers.md)); they are not in the public repositories
  yet.
- **HTTP**: gzip and deflate on both listeners for the answers that hold no secret
  ([ResponseCompression.md](hmailserver/docs/ResponseCompression.md)).
- **Storage compression and attachment de-duplication** (schema 6098): with `MessageStoreCompression` on (off as
  shipped), a message delivered or appended is stored deflated, and each MIME part of at least
  `MessageStoreDedupMinKB` (256 KB) is kept once for every message that carries the same bytes. Every reader sees
  the message as it arrived, and a stored file is checked by its SHA-256 before it is served; the queue is never
  compressed. `--measure-store` says what it would save and changes nothing, `--compress-store` and `--compress-store --decompress`
  rewrite what is stored, and `GET /api/v1/store`, the Deck's dashboard and the Control Panel's Performance page
  say what it saves. With it on, `Message.Filename` over COM names the stored form
  ([MessageStore.md](hmailserver/docs/MessageStore.md)).
- **Object storage for the message store**: messages on an S3-compatible store (AWS S3, MinIO, Wasabi, Backblaze
  B2, Ceph RGW) or Azure Blob Storage, with the database as the index. A message is written to the local disk first
  and moved into the store by a background committer, so a store that cannot be reached delays nothing and loses
  nothing; IMAP and POP3 are told to try again for a message only the store holds. TLS to the store is always
  verified. `--copy-store-to-object-storage` and `--copy-store-from-object-storage` copy both ways, verified and
  resumable. Off until an administrator names a store: `MessageStoreBackend` is `file` as shipped, and a server
  with no store reads and writes as before ([ObjectStorage.md](hmailserver/docs/ObjectStorage.md)).
- **Event scripts on Linux**, in JavaScript, in the QuickJS engine (Fabrice Bellard's release 2025-04-26, vendored
  as source), for the same events under the same function names with the same objects; `ScriptLanguage` takes a
  third word, `JavaScript`. Until now a Linux server refused any script. Windows keeps VBScript and JScript and
  refuses JavaScript at load. A script has no file, process or network access, and `ScriptTimeout` and a 64 MB
  ceiling bound it ([EventScripts.md](hmailserver/docs/EventScripts.md)).
- **SNMP**: a read-only SNMPv3 agent (authPriv only: HMAC-SHA-256 and AES-128, with SNMPv2c optional and off)
  answering the mail MIB (RFC 2789) and the standard groups from the same counters `/metrics` serves. Off, and
  answering nobody until a manager's address is in `SnmpAllowedAddresses`; its settings are on REST and applied at
  a restart, with no COM property or Control Panel page. Its enterprise arc is RFC 5612's documentation number, not
  one registered to this project ([Snmp.md](hmailserver/docs/Snmp.md)).
- **The live update's files are held on Windows.** Beside what the update-feed item says, a repaired `Updates`
  folder is emptied, one owned by another account is refused (`HM6894`), the folders are held open through the
  apply so that nothing above them can be swapped, the service's own SID is given Modify there, and the update
  helper loads DLLs from System32 alone ([UpdateFeed.md](hmailserver/docs/UpdateFeed.md)).
- **The Windows installer**, after its first smoke test on a clean Windows Server 2025: an upgrade no longer writes
  a Critical `HM5011` event to the Application log; the uninstaller waits for the service's process to end before
  it deletes anything, so `hMailServer.exe` and its DLLs are no longer left in `Bin`, and a file still in use is
  removed at the next restart; the event log source is removed; and an uninstall after an upgrade no longer leaves
  the COM AppID behind.
- Fixed: an administrator password with a space, a double quote or backslashes before one failed a new
  installation at the database step, and a silent upgrade that moved the schema, with exit code 3: the installer
  put it unquoted on `DBSetupQuick.exe`'s command line, and DBSetupQuick on `DBUpdater.exe`'s (`DBSetup.exe`,
  run for an external database, asked for it again). Both hand it on in the environment variable
  `HM_ADMIN_PASSWORD` now, the installer to `DBSetup.exe` too, as it already did for `hMailServer.exe
  --set-admin-password`, so it is on no command line. An unattended install may give the installer the password
  in `HM_ADMIN_PASSWORD` in place of `/adminpassword=`, which cannot carry a double quote (Inno Setup drops it)
  and which anybody listing processes can read; it is set in an elevated prompt, since it does not cross the UAC
  prompt ([Upgrading.md](hmailserver/docs/Upgrading.md)).
- **The product points at GitLab**: the Control Panel's About and stalled-mail links, the `Policy:` line of the
  `security.txt` the server serves, the systemd units' `Documentation=` and the Linux packages' homepage. The
  update feed field is labelled "Update feed URL" in all eighteen catalogues. So do the Arch Linux PKGBUILD's `url`
  and the release tag it clones, the Helm chart's home and sources, and the winget and Chocolatey templates'
  addresses and installer, which is the one in GitLab's package registry. The container image is named
  `registry.gitlab.com/progressiverobot/hmailserver` in the Compose file and the chart; nothing is published there
  yet (above), and the images published on `ghcr.io` before the suspension answer 404. The update check no longer
  sends GitHub's `X-GitHub-Api-Version` header. The release tags' signing keys are in `.gitlab/allowed_signers`,
  which was `.github/allowed_signers`, and the VEX document is `.gitlab/hmailserver.openvex.json`.
- **The project on GitLab**: issue templates for a bug, a feature, a question and a security report, the first
  three asking for the version, the platform, the database and a log excerpt; a merge request template; the
  Service Desk, an address for anyone without a GitLab account, whose message becomes a confidential issue
  (README's Getting help, [SUPPORT.md](SUPPORT.md)); the releases as an Atom feed; and the README's list of where
  each of these is. [GOVERNANCE.md](GOVERNANCE.md) gives the maintainers' roles on GitLab, and
  [Mirroring.md](hmailserver/docs/Mirroring.md) how to push-mirror the repository to a second forge, and the
  weekly export of the issues, merge requests, releases and wiki that a mirror does not carry; neither is set up
  yet.
- Fixed on Linux: a character-set conversion that failed read an unterminated buffer, where Windows had
  substituted; it now gives an empty string.
- Fixed on Linux: the name of the account the service runs as was read with `getpwuid`, whose answer is one
  buffer for the whole process, while the setup checklist, auto-ban's firewall check and the live update ask for
  it from threads of their own, so two at once could read each other's entry or a torn one. It is read into a
  buffer of the call's own (`getpwuid_r`).
- Fixed: the container image built from `hmailserver/source/Server/platform/packaging/docker/Dockerfile` installed
  the `.deb` without its two Recommends, so it had no 7-Zip - a backup failed with "Could not add the backup
  settings to the archive" - and no Python 3 for `hmctl`. It installs `7zip` and `python3` with the package.
  No image of 6.3.4 is published (above); this is the image built from the file.

### Build and tests

- The native server's line coverage was measured for the first time: 73.48% over the whole Windows regression
  suite. The three MIME fuzz harnesses gained a nightly job, and the Linux regression suite was split in two
  halves; both ran on GitHub Actions until 18 September 2026, and on GitLab CI the fuzz job is `linux-fuzz`, nightly once the owner creates its schedule,
  and the Linux suite is one job, `linux-suite`, on the project's own runner (see the CI item below).
- Fixed: the MIME fuzz harnesses did not build. Since encryption at rest, a message the MIME code cannot parse is
  kept aside in "Problematic messages", which calls `FileUtilities::Exists` and `CreateDirectory`, and the
  harness's stand-in for `FileUtilities` lacked both, so `linux-fuzz.sh` and `build-fuzz.ps1` stopped at
  `Mime.cpp`. It gains both, answering false like its other writers, so the harness still writes nothing; the
  parser is unchanged. Found by 6.3.4's release fuzz run.
- Twenty-one new checkers under `build/` hold the rules the batches added, among them `check-account-stores.py`
  (what a backup must carry), `check-com-error-info.py` (COM refusals a caller can read),
  `check-collection-iteration.py` (locked iteration over shared collections), `check-com-dispids.py`,
  `check-openapi-blocks.py` and the audit and role choke-point checks.
- The three GnuPG proofs of the gateway-encryption and zero-access fixtures run on Windows for the first time:
  they had handed Git for Windows' gpg a home directory it could not read.
- A load generator in the repository, `build/perf/load/loadgen.py`: messages per second in and out, IMAP sessions
  per core under a latency limit, memory per session and Dovecot's imaptest as a benchmark, against any server,
  with the machine's details in every result; and a lab recipe that runs it against hMailServer and against
  Postfix and Dovecot on the same machine under stated fairness rules
  ([PublishedNumbers.md](hmailserver/docs/PublishedNumbers.md)). No numbers have been measured with it yet.
- **Windows on ARM, compiled but not run.** A `Release|ARM64` configuration in the server's three projects,
  `-Platform ARM64` in `build.ps1`, `build-tools.ps1`, the three library scripts, `get-installer-binaries.ps1` and
  `get-dotnet-runtime.ps1`, and a second installer, `hMailServerARM64.iss` (`hMailServer-<version>-arm64.exe`),
  sharing the x64 installer's sections. The ARM64 server uses SQL Server or PostgreSQL; it has no built-in
  database, since SQL Server Compact has no ARM64 build, and no bundled MySQL client. An ARM64 server's update
  check looks for the ARM64 installer, and `VersionArchitecture` says `ARM64`. It was first compiled on
  26 September 2026, on the x64 development machine: the three ARM64 libraries, the server, the tools and an
  ARM64 installer all built, every native image an ARM64 one. That compile found two faults, both fixed for ARM64
  only: libpq's cross build wanted a time zone compiler that runs on the build machine, and is now built with
  the system's time zone data, which libpq never reads; and the tools were compiled against the x64-marked COM
  wrapper, which .NET will not load into an ARM64 process, so `generate-com-wrapper.ps1 -Platform ARM64` makes a
  machine-neutral one, `build-tools.ps1 -Platform ARM64` points the tools at it through the `HmInteropDirectory`
  property and a new `Tools/Directory.Build.targets`, and the ARM64 installer ships it. Nothing of it has run:
  the project has no ARM64 hardware, and 6.3.4 carries no ARM64 installer; the x64 build, its COM wrapper and
  its SBOM are unchanged. `hmailserver/docs/WindowsOnArm.md`.
- **CI on the project's own GitLab runners.** `.gitlab-ci.yml` runs the platform-neutral checkers, the Deck's and
  the webmail's script harnesses and, for a merge request from a fork, a sign-off check on GitLab's shared runners;
  and, for a push to `master`, a `batch*` branch or a `v*` tag only, the Linux build, its tester programs, the
  whole Linux suite, the packages, the container image, the cross-compile for AArch64, the database upgrade gates
  and the SBOMs on `linix`, and the Control Panel, the tools and the whole Windows suite on `bench150`. A nightly
  pipeline fuzzes the three MIME harnesses once the owner has created its schedule. Nothing reached from GitHub
  is used. Until 26 September 2026 only the update feed's `pages` job had run there, and the ported Linux scripts
  were run by hand on `linix`. The first pipeline of the whole file ran on `master` at `40c7d3e0c` that day
  (pipeline 2884863496), and it finished failed. The checkers, the Linux build and its packages, image,
  cross-compile, sanitizer build and SBOMs, and the update feed passed; `harnesses` and `update-helper` failed on
  the shared runners, `linux-suite` and both database upgrade gates on `linix`, `windows-gate` and
  `windows-dotnet` on `bench150`, and `dependency-scan`, `repo-hygiene`, `semgrep` and `static-analysis`, which
  may fail without failing the pipeline. Each failure was the tree's or the job's, not a fault of the server's, but
  for two of the findings `static-analysis` reported; and 6.3.4 fixes each but that job's: the helper job checked
  the systemd units on a checkout the runner makes world-writable; the portal harness depended on the runner's time
  zone; the CI image had no 7-Zip, which the suite's backup tests need; three tests assumed the SMTP port, the
  order two messages land in, and an INI section to be a setting; the upgrade gate wound a new database back to
  6029 by reading the later steps, and now starts from the create scripts of 6029 themselves and compares what it
  upgrades to with a new database, which found two columns of `hm_bouncecounts` the create scripts left without the
  default the 6055 step gives them (they now have it); GitLab refused two scans' reports, each two files of one
  type, and each now uploads one; `repo-hygiene` found six images not pinned by digest and a password written in
  the file for the upgrade gate's own database, and judged a job by whether it extends `.own-runners` rather than
  by the rules it runs by; and each finding Semgrep reported beyond its baseline is fixed, or suppressed with the
  reason. Of the findings `static-analysis` reported beyond its baseline, the two that were defects of the server's
  are fixed (above); the others have not yet been read into its baseline, so that job still fails. The owner's edit
  of the README on `master` started a second pipeline, which failed on `harnesses` the same way. No pipeline has
  run on the fixed tree yet ([ContinuousIntegration.md](hmailserver/docs/ContinuousIntegration.md)).
- **The CI after its first review on GitLab.** The fuzz, Renovate, whole-history secret scan and export pipelines
  are chosen by their schedule's description - `fuzz`, `renovate`, `security`, `export` - since the project
  refuses pipeline variables and GitLab counts a schedule's as such; only the update feed's schedule exists yet.
  A later push no longer cancels the Linux suite or the Windows gate half-way, and a push that changes only
  Markdown at the repository's root or under `.gitlab/` starts nothing on the project's runners. A busy bench is
  a warning, except in a release tag's pipeline. The fuzz job's findings are its artifact, for Developers only,
  and kept out of the public job log, where an input could be an exploit. cosign's image is pinned by
  digest (2.6.5). GitLab's own SAST (`semgrep-sast`, which adds the server's C and C++) and secret detection
  (`secret_detection`, which fails on a finding; `.gitleaksignore` names the thirty findings of a scan of the
  whole history and the twelve the landing's scan found in this release's own commits, none of them a secret) run
  on the shared runners for `master` and merge requests.
  `forge-export` writes the weekly archive [Mirroring.md](hmailserver/docs/Mirroring.md) describes, and
  `package-repos` the apt and dnf repositories, each waiting for the owner's token or key.
  `build/check-release-chain.py` holds all of it, the release jobs and the container image's gate included, and
  `build/check-forge-export.py` the export.
- **Changes land through merge requests, signed off, from the first landing after 6.3.4.** A batch reaches
  `master` through a GitLab merge request,
  by fast-forward, once its branch's pipeline has passed and every thread is resolved: `build/land-via-mr.py`
  checks the batch, pushes it, opens the merge request, waits for the pipeline, merges without squashing, and
  records on the merge request whether somebody other than the author approved; without that approval - Zain Ul
  Abidin's, or the owner's for Zain's own, given on the tip it lands - it refuses to merge, unless the owner
  overrides it for one landing with `--land-without-approval "<reason>"`, which it records. From the first batch
  landed this way, every commit that reaches `master` carries its author's `Signed-off-by`, the maintainers'
  included: the `sign-off` job checks every merge request and every `batch*` branch
  (`build/sign-off.py`, which the landing script uses too), and Renovate signs its commits off. `master`'s rule is
  to take no direct push once the first batch has landed this way (`hmailserver/docs/ContinuousIntegration.md`,
  "Landing on master"). No batch has landed this way on GitLab yet; both scripts' self-tests run in `checkers`.
- **Nothing is downloaded from GitHub to build**: the seventeen GitHub workflows, Dependabot and the CodeQL
  configuration have left the tree and are in its history; `libraries\build-openssl.ps1` takes OpenSSL's release
  archive from Debian's archive, `openssl_<version>.orig.tar.gz`, which is OpenSSL's own tarball, held to the same
  pinned SHA-256, or from `-Archive`; `build-appimage.sh` uses linuxdeploy only from `TOOLS_DIR` and otherwise
  refuses; winflexbison comes from SourceForge; and the installer's build inputs come from GitLab's package
  registry.
- **Security scanning** on the same runner, in place of CodeQL, dependency review and Scorecard: cppcheck and
  clang-tidy held to a committed baseline, Semgrep, Trivy and OSV over the lock files, the CI image and the
  vendored libraries, and the repository checks Scorecard ran. They hold a release, not a change (next item).
- **A release is held by its security scans, and a secret is refused before it is pushed.**
  `build/ci/security-gate.py` reads what `static-analysis`, `semgrep`, `semgrep-sast` and `dependency-scan` found
  in the release's commit, and the release job - or, for a release made by hand, its runbook - refuses while one
  of them did not finish or found something `SECURITY.md`'s policy counts that is in no baseline and is not
  accepted, for at most a year, in `.gitlab/security-exceptions.yml`, which the owner keeps on `master`. That file
  accepts 455 findings at the release - 188 of GitLab's Semgrep analyzer, 263 of the static analysis and 4 of
  Semgrep's open rules, from the scans of `40c7d3e0c` and of the release tree - each read by two review agents
  under the owner's delegation and each with the reason it is not exploitable; the seven it found real are fixed
  in this release (under Security and identity). `build/scan-secrets.py` runs gitleaks with GitLab's rules in the
  two git hooks the repository ships (`build/hooks`, turned on with `git config core.hooksPath build/hooks`) and
  in the maintainer's landing, which pushes nothing while it finds a secret or cannot scan
  ([SECURITY.md](SECURITY.md), [CONTRIBUTING.md](CONTRIBUTING.md), "Secrets").
- **Dependency updates by Renovate** on the same runner, with nothing read from GitHub; inert until the owner
  creates its token and schedule.
- **A test lab on `linix`**: MySQL 8.4, MariaDB 11.4, PostgreSQL 17 and SQL Server 2022, Keycloak for the OpenID
  Connect and SAML sign-ins, Postfix and Dovecot as peers, Dovecot's `imaptest`, and Uptime Kuma
  ([TestLab.md](hmailserver/docs/TestLab.md)). Its first runs found the unencrypted MySQL connection, the
  certificate check that never read an iPAddress entry and the IMAP answers fixed above.
- **The installer smoke test** installs, upgrades and uninstalls on a throwaway Windows Server 2025 VM, from a
  snapshot, checking what the installer promises for each (`build/ci/installer-smoke.ps1`); its first run, of 6.3.2
  upgraded to 6.3.3, found the four installer defects fixed above.
- **An SBOM for every compiled file a release carries**: from 6.3.4, the Windows installer, the `.deb` and the
  `.rpm` each go out with `<file>.cdx.json`, a CycloneDX 1.6 document whose subject is that file by its SHA-256
  and SHA-512 and whose components are what the file holds and needs, made by `build/asset-sbom.py`, which also
  checks one against its file. The five GitLab releases before 6.2.3 that carry an installer carry one too since
  26 September 2026, made from each installer ([SECURITY.md](SECURITY.md), "Software bills of materials").
- **Every build input pinned and named**: the .NET Desktop Runtime the installer bundles is pinned to 10.0.12 by
  version and SHA-512, where it was whatever Microsoft's 10.0 link gave on the day; the SBOM names seventeen
  components, among them zlib, the installer's fetched files and the Linux packages' system libraries, and
  `build/check-sbom-inputs.py` fails when a build input is missing from it.
- New checkers: `check-message-store-seam.py`, `check-com-shared-copies.py`, `check-mysql-charset.py`,
  `check-installer-scripts.py`, `check-ci-image.py` and `check-sbom-inputs.py`.

Full notes: [the 6.3.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.4).

---

## [6.3.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3) - 2026-09-15

Large outbound messages sent with CHUNKING no longer stall, databases older than 6.3.2's schema upgrade again,
the webmail was rebuilt to the shape of Gmail and Outlook.com, the account's address book is served over CardDAV,
and the browser Control Deck was measured against the desktop Control Panel and brought close to it.

**Delivery**

- An outbound BDAT chunk larger than one 60,000-byte send buffer no longer stalls (GitHub issue 261): every
  message over 60,000 bytes to a server advertising CHUNKING had stalled since outbound BDAT arrived.
- The full-text indexer no longer reports an error for a message deleted before its terms were saved.

**The webmail**

- Rebuilt to the shape of Gmail and Outlook.com: one search box, a navigation column, a reading pane right, below
  or off, Undo after every move, a compose window that docks or pops out, and replies written under the message.
- Pin (`$Pinned`), mute (`$Muted`), block a sender, a right-click menu and the inbox in tabs, Gmail's or
  Outlook's way, decided by the server from the headers; follow-up flags with due dates, nudges and quick steps.
- The last ten searches offered again, contact names completing to `from:`, and fourteen more search operators.

**CardDAV**

- The account's address book on phones and desktop clients (RFC 6352) on the web-services HTTPS listener, with
  the card a client sends stored as sent (schema 6040) and the IMAP logon's lockout and auto-ban.

**The Control Deck and REST**

- `build/check-deck-parity.py` measures the desktop Control Panel against REST and the Deck; of 330 properties,
  328 became writable over REST and 322 reachable from a Deck view on 14 September.
- New write routes for the anti-virus settings, account and domain fields, lists, the anti-spam lists, incoming
  relays and, under an account's address, its app passwords, folder permissions and messages.
- A domain's relay password could not be saved on any Windows installation, because a DPAPI envelope is longer
  than the column; schema 6039 widens it.

**Security, from the release's own review**

- The REST routes for the `[Settings]` section of `hMailServer.ini` answered to any API key, so a read-only key
  could read its secrets; they answer to the administrator password only. A key restricted to a domain could
  lift that domain's limits; it is refused them with 403. CardDAV's request handling is bounded.

**Schema:** 6038 to 6040 (6039 widens a domain's relay-password column; 6040 adds the CardDAV tables).

**Upgrading**

- 6.3.2 could not upgrade a database older than its own schema (GitHub issue 263): every upgrade from 5.x or
  6.2.x stopped with "The server has not loaded its configuration". From anything older than 6.3.1, take 6.3.3;
  nothing in the database was changed by a failed 6.3.2 attempt.
- A live update started from an open Control Panel failed with the installer's exit code 5. It is fixed, but the
  helper that runs an update is the one already installed, so on 6.3.1 or 6.3.2 update by hand once: close the
  Control Panel and run this installer.
- On PostgreSQL a backslash is now stored as one backslash; values already stored doubled stay as they are, so
  edit and save them once.
- Until you upgrade, `OutboundChunking=0` is the workaround for the BDAT stall.

Full notes: [the 6.3.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.3).

## [6.3.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2) - 2026-09-13

The webmail became a mail client, Kerberos single sign-on arrived on all three protocols, an auto-ban reaches the
operating system's firewall, the Linux packages install on the next distribution release, and thirty-seven
binaries left the repository.

**The webmail**

- Reading: conversations by thread; Archive, Junk and Trash on one key each, with a move into or out of Junk
  teaching the spam filter as an IMAP `MOVE` does; a search syntax with operators; labels as IMAP keywords
  (schema 6036) that every IMAP client sees; image attachments shown and PDFs previewed; warnings for a link
  whose text names another site and for a sender named like a contact.
- Writing: a formatting bar; a From picker offering exactly what SMTP submission would accept; signatures,
  templates, undo send, read receipts (RFC 8098) and one-click unsubscribe (RFC 8058) to public addresses only;
  send later and snooze (schema 6034); an address book collected from what the account sends (schema 6032);
  large files sent as links that expire (schema 6035).
- S/MIME in the browser with the Web Crypto API, the private key wrapped under a key derived from the account
  password (schema 6037), which the server cannot open.
- Settings, Away, Filters (a rules editor that writes Sieve), Contacts, Storage, Files and Security pages; the
  seventeen Control Panel languages plus Greek, Korean and European Portuguese; installs as an app and reads
  the inbox offline; branding per server or domain.

**Authentication**

- SASL GSSAPI (RFC 4752) on SMTP, IMAP and POP3 on Windows, off unless `GssapiEnabled=1`.
- `AutoBanFirewall=1` puts a banned address into Windows Defender Firewall; on Linux a packaged hook keeps an
  nftables set, with a fail2ban filter and jail; `AutoBanCommand` and `AutoBanNeverBan` beside it. All off.
- Making an app password requires the account's own password, and never accepts an app password for it.
- A SURBL server's answers are judged against an expected result (schema 6038); without one, the codes with
  which Spamhaus refuses a query are no longer taken as listings (GitHub discussion 167).

**Linux**

- Boost is linked statically, so the `.deb` installs on Ubuntu 26.04.
- A container image from every tag, whose entrypoint writes the database section, the REST listener and the
  administrator password from the environment and creates or upgrades the database.

**Database**

- The 6029 to 6030 step sweeps parents before children, fixing the orphan-sweep defect 6.3.1 named, in all four
  backends.
- Every statement in a MySQL script is followed by a blank line, and `build/check-sql-separators.py` keeps it so.

**Build**

- Thirty-seven of the forty committed binaries left git; `build/get-installer-binaries.ps1` fetches them from
  the `build-inputs-1` release against the SHA-256 in the manifest, and the COM wrapper is generated.
- A server holding a database whose schema it refused (HM5011) answers COM with the refusal instead of an access
  violation.

**Schema:** 6031 to 6038 (6032 to 6037: contacts, account preferences, scheduled sends and snoozes, files sent as
links, message keywords, S/MIME keys; 6038: the SURBL expected result).

**Known afterwards:** 6.3.2 could not upgrade a database whose schema was older than its own (GitHub issue 263),
and its live update from an open Control Panel failed; 6.3.3 fixes both.

Full notes: [the 6.3.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.2).

## [6.3.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1) - 2026-09-11

The first release whose Windows installer carries an Authenticode signature. Nothing in the server changes: the
compiled server differs from 6.3.0 by its version stamp and one comment line.

**Signing**

- The installer is signed with Azure Artifact Signing against a certificate issued to Progressive Robot Ltd, so
  the elevation prompt names the publisher and a policy that refuses unsigned binaries no longer refuses it.
  SmartScreen still warns until reputation accumulates.
- Only the Windows installer is Authenticode-signed; the Linux packages have no such signature, and the
  Sigstore bundle beside every asset is unchanged.
- `UpdateRequireAuthenticode=1`, in the server's update settings since 6.2.28, becomes usable on Windows,
  because the installer it downloads now carries a signature.

**The release path**

- The release workflows were corrected before the certificate was first used: the signing gate checks all six
  of its settings, every attached asset is verified as it stands on the release, both SBOMs are required before
  anything is signed, an RFC 3161 countersignature is required, and runs for one tag are serialised.

**Documentation**

- The README offers the Linux packages on its download line; two files that named a version 6.2.29, which never
  existed, now say 6.3.0.

**Schema:** 6031, unchanged.

**Known, and fixed in 6.3.2:** the orphan sweep of the 6029 to 6030 step ran children before parents, so an
upgrade from below 6030 of a database that already held orphaned rows could refuse its own foreign keys. It
failed loudly and rolled back.

Full notes: [the 6.3.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.1).

## [6.3.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0) - 2026-09-10

hMailServer runs on Linux: a mail server that had been Windows-only since 2002 builds, installs, runs and delivers
mail on x86-64 and AArch64 Linux from the same source tree, with the Windows build unchanged in what it compiles
and how it behaves. Because a Linux server has no Control Panel and no COM, the REST API gained a large write
surface and the browser Control Deck drives most of it.

**Linux**

- Every core translation unit compiles under clang on x86-64 and AArch64, and the whole core builds and links
  with GCC on x86-64; `build/linux-tu-census.sh` counts them file by file.
- A `.deb` and an `.rpm` for both architectures, a `PKGBUILD` for Arch and an AppImage, each with a systemd unit
  that runs the server as its own user, a logrotate rule, the configuration under `/etc/hmailserver` and the
  Control Deck.
- `hmailserver --create-database`, `--upgrade-database`, `--set-admin-password` and `--check-config`.
- Proven against PostgreSQL 18 and MariaDB 11.8 from schema creation to a message delivered and read back over
  IMAP; directory authentication against slapd over StartTLS and LDAPS; stored secrets under AES-256-GCM with a
  key file the installation owns, where Windows uses DPAPI.
- The same commit built twice on the hosted runners gave byte-identical binaries.

**REST writes what only COM could write**

- `PUT /api/v1/settings` with its anti-spam and logging groups (108 settings, each through the Control Panel's
  own setter), global rules, SMTP routes, aliases, accounts with their password and administrator level,
  certificates and the listeners that bind them, and `POST /api/v1/server/reinitialize`.
- Refused to a read-only key and to a key issued for other domains; the OpenAPI document is valid JSON for the
  first time.
- Not yet written over REST: a domain's properties beyond its active flag and postmaster, DKIM signing, the
  per-domain size limits, the domain signature and a per-domain relay host.

**The Control Deck**

- Editors generated from the server's own OpenAPI document; rules, routes, certificates and listeners created,
  edited and deleted; accounts created and deleted; the services restarted in place.
- It no longer keeps the administrator password: `POST /api/v1/session` exchanges it once for an `HttpOnly`,
  `SameSite=Strict` session cookie that ends when the password changes, and a write must carry
  `X-Requested-With`.

**The webmail**

- The portal at `/portal` became a webmail: addresses for folders and messages, the Control Deck's look, new
  mail noticed without a reload, folders created, renamed and deleted, and inline images shown without loosening
  the frame's sandbox. `build/check-portal-script.py` runs the page's script in CI.

**Under the surface**

- Non-ASCII folder names and non-ASCII command lines are handled correctly on Linux.
- A TLS key-exchange group list OpenSSL rejects is reported once, not for every listener and delivery.
- The administrator's user name is no longer translated in the Control Panel's sign-in box (GitHub issues 156
  and 177); an installation-paths diagnostic lists every configured directory (GitHub issue 158).
- A Linux regression suite, `hmailserver/test/LinuxRegressionTests`, compiles 260 of the suite's own files in
  place and runs them against the Linux server.

**Schema:** 6031, unchanged.

**Upgrading**

- Windows: run the installer over the existing installation. Nothing new is on by default; the Control Deck and
  the portal need `RestApiPort`.
- Linux, first installation: install the `.deb` or `.rpm`, then `hmailserver --create-database`,
  `hmailserver --set-admin-password` and `systemctl enable --now hmailserver`.
- Moving a Windows installation to Linux is not a supported migration.

**Known and unfixed:** the installer is not Authenticode-signed (6.3.1 is the first that is); per-domain DKIM
cannot be configured on Linux; the Linux regression run skips what no route reaches yet.

Full notes: [the 6.3.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.3.0).

## [6.2.28](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28) - 2026-09-08

The largest release of the fork so far: the server can update itself, an account can use its mailbox in a
browser, and the Control Panel speaks seventeen languages, with a real HTTP/1.1 server under the REST API and the
web services. Everything new is off until it is turned on.

**Live update**

- `UpdateCheckEnabled` (0 by default) reads the project's release feed every `UpdateCheckHours` (24); fetching
  and applying an installer are two further opt-in steps.
- What is applied is verified against the release's Sigstore bundle, which cannot be turned off; the settings
  `UpdateTrustRootsFile`, `UpdateSigningIdentity`, `UpdateSigningIssuer`, `UpdateSourceRepository` and
  `UpdateLogPublicKeyFile` point it at a private Sigstore instance.
- `hMailServer.Updater.exe` runs the installer with the service stopped, waits `UpdateServiceWaitSeconds` (180)
  and reinstalls the previous version if the service does not come back; `UpdateBackupBeforeApply=1` refuses an
  unattended apply without a successful backup.
- `HttpProxy=host:port` sends every web request the server makes as a client through a forward proxy.

**The mailbox in a browser**

- `/portal` on the REST listener, and `/api/v1/me` behind it, answer to an account's own credentials only: read,
  search, flag, move and delete; send with attachments, drafts, reply, forward and Bcc; the account's quarantine,
  forwarding, signature, vacation reply, Sieve script and password.

**Under it**

- The REST API and the web services run on `HttpServer`: HTTP/1.1 on Boost.Asio with keep-alive, chunked bodies
  and header and body limits.
- IMAP `COMPRESS=DEFLATE` (RFC 4978), behind `IMAPCompressionEnabled`.

**The Control Panel and the Control Deck**

- The Control Panel in Czech, Danish, German, Spanish, Finnish, French, Italian, Japanese, Norwegian bokmål,
  Dutch, Polish, Brazilian Portuguese, Russian, Swedish, Turkish, Ukrainian and Simplified Chinese, beside
  English: 3,376 texts, chosen in the connect card and remembered per user.
- The Control Deck reads the settings, the log files, the certificates and the global rules.

**Fixed**

- A masked password was typed backwards from the second character (GitHub issue 156).
- Saving the SSL/TLS page dropped the post-quantum key exchange groups, and saving the Scripting page took every
  COM object from every script.

**Schema:** 6031, unchanged.

**Upgrading:** this is the first release that carries the live update, so the upgrade to it is manual.
`UpdateCheckEnabled`, `RestApiPort`, `IMAPCompressionEnabled` and `HttpProxy` are all opt-in; a server upgraded
without touching its settings behaves as it did on 6.2.27.

**Known and unfixed:** the Control Deck reads but does not write, and holds the administrator password in
`sessionStorage` while it is open; the regression suite runs on Windows only.

Full notes: [the 6.2.28 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.28).

## [6.2.27](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27) - 2026-09-07

Two roadmap items open since the roadmap was written: the validating resolver proves a missing DS record missing
rather than assuming it, and an external IMAP account can mirror every folder of a remote mailbox.

**Security**

- A DS query answered with nothing is no longer taken as an unsigned delegation: the resolver requires the
  parent's signed NSEC or NSEC3 proof (RFC 4035 section 5.2, RFC 5155 section 8), and a delegation without one
  under a signed parent is Bogus. `Diagnostics.DnssecChainStatus` reports a chain.

**Mail flow and migration**

- `FetchAccount.MirrorFolders` (schema 6031) mirrors every folder of a remote IMAP mailbox byte for byte, with
  flags and internal dates, without delivering anything through the filters; *days to keep messages* 0 makes it
  a move.
- The Import Tool reads a Maildir, with the INBOX, every Maildir++ folder and the flags in the file names.
- `hmailserver/docs/Migration.md` describes each route into this server and what it keeps.
- A message imported into a folder a client has open now appears at once.

**Schema:** 6030 to 6031 (one column on `hm_fetchaccounts`).

Full notes: [the 6.2.27 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.27).

## [6.2.26](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26) - 2026-09-06

The release to take if you run the built-in SQL Server Compact database. Nothing else in the server changes.

**Fixed**

- Every SQL Server Compact upgrade through schema 6030 was reported as failed after it had succeeded, and the
  check that gave the false verdict took the service down with it (GitHub issue 114). The four verification
  probes for the foreign keys crashed the Compact Edition provider; they are rewritten so that they run on every
  backend.
- `build/check-db-scripts.ps1` now executes every probe against a freshly created database, with a negative
  control, and DBUpdater's message gives the backend's own words.

**Schema:** 6030, unchanged.

**Upgrading:** if the 6.2.25 installer reported a failed upgrade, the database is already at 6030 with its
foreign keys, and this installer finds nothing left to do.

Full notes: [the 6.2.26 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.26).

## [6.2.25](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25) - 2026-09-06

The release to take if you use the built-in ACME client, and sixty commits of new work besides.

**The fix to upgrade for**

- ACME issuance and renewal ended the process (GitHub issue 93): two calls handed the OpenSSL DLL a C runtime
  `FILE*`, and OpenSSL answered the missing `OPENSSL_Applink` by terminating the process. Both calls use
  OpenSSL's own file I/O, the certificate is deployed before the DANE TLSA line is computed, and a certificate
  6.2.24 issued before it died is deployed at the first ACME check.
- Two restarts of the servers at once could end in an access violation; restarts run one after the other.

**Mail flow**

- External accounts over IMAP (`FetchAccount.ServerType` 1), the INBOX only.
- Message retention per domain and per account (schema 6027); an archive index with legal hold (schema 6029).
- Outbound delivery pipelines the envelope and sends the message as one BDAT chunk when the remote advertises
  PIPELINING and CHUNKING (`OutboundPipelining`, `OutboundChunking`, both on).
- Delivery hard links (`DeliveryHardLinks`, off); `SEARCH BODY` and `TEXT` read text attachments.
- A Message-ID is added only for submissions (upstream PR 552); a stranded fetch account is released
  (upstream PR 603).

**IMAP**

- Every session numbers its selected folder for itself (upstream PR 602); sessions stop sharing unguarded state
  (upstream PRs 580 and 566); default special-use folders at account creation (schema 6026, off); one
  authorisation choke point for folder access.

**Security and authentication**

- SASL EXTERNAL with client certificates; the AUTH line limit raised to 12,288 octets.
- OAuth2 keys from the provider's JWK Set and token introspection; scrypt (`PreferredHashAlgorithm=7`); the
  password-hash work factor as settings (upstream PR 554); tarpits on failed logons and stranger recipients.
- The administrator's second factor is enforced by the server; `SmtpAuthenticatedSenderCheck` and
  `ScriptAllowedObjects`; the Apple configuration profile served over HTTPS only.

**Administration and databases**

- The REST API reads what was COM-only; a week of metric history (schema 6028); every backup proves its mail
  can be restored (`BackupVerifyRestore`, on); Control Panel accessibility for High Contrast, mnemonics and
  screen readers.
- TLS to PostgreSQL and MS SQL as `[Database]` settings; a plaintext MySQL only by choice (upstream PR 559);
  seventeen foreign keys (schema 6030).
- OpenSSL 4.0.2 and Boost 1.92.0; twelve upstream ports in all.

**Schema:** 6025 to 6030 (6026 the special-use folders switch, 6027 retention, 6028 metric history, 6029 the
archive index, 6030 seventeen foreign keys, which first removes orphaned rows).

**Upgrading**

- The Apple `.mobileconfig` profile is served over HTTPS only; a Message-ID is no longer added to relayed mail;
  the four HTTPS clients require TLS 1.2 or later; AUTH lines may be 12,288 octets; IMAP sequence numbers are
  stable within a session; `Account.DeleteMessages` keeps a designated special-use folder.
- Every new setting is off or inert except `OutboundPipelining`, `OutboundChunking` and `BackupVerifyRestore`.
- COM clients compiled against the 6.2.24 type library keep working.

Full notes: [the 6.2.25 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.25).

## [6.2.24](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24) - 2026-09-04

The first stable release since 6.2.21: the code of 6.2.23 Alpha 2, cut the same day, plus NuGet lock files for the
.NET tools and the CI changes that hold every restore to them. It brings everything the 6.2.22 and 6.2.23
pre-releases introduced to a 6.2.21 installation at once.

**Since 6.2.21, in short**

- Mail that was silently lost is not any more: a delegated `APPEND`, `COPY` or `MOVE`, Thunderbird's Sent copies
  with `UTF8=ACCEPT` (GitHub issue 53), a kept-open log never flushed (GitHub issue 33), and an ACME client that
  had never worked against real Let's Encrypt (GitHub issue 34).
- Anti-spam bypasses closed: a forged header steering a per-account override, a HELO literal choosing the
  address the IP tests ran against, and a transaction opened without a greeting.
- Shared and delegated mailboxes, full-text search, RFC 3464 bounces, BINARYMIME, per-account spam thresholds, a
  sender blacklist, domain-wide out-of-office, list moderation, the Windows Event Log, OTLP metrics and logs, the
  PROXY protocol, an HTTP filter hook, eleven IMAP extensions, Microsoft 365 XOAUTH2, app passwords, TOTP, a
  quarantine, password history, message trace, per-domain relay and a disk-space floor.
- Control Flow Guard, a reproducible build, signed release tags, SBOMs and Sigstore bundles.

**Schema:** 6025, unchanged since Alpha 1.

**Upgrading from 6.2.21**

- The schema moves from 6011 to 6025 in fourteen steps, one way. The 6024 to 6025 step rewrites
  `hm_messages.messageflags` on MS SQL, SQL Server Compact and MySQL, which can take long on a large database;
  PostgreSQL is unaffected.
- Without a switch: `MAIL FROM` or `AUTH` straight after STARTTLS without a fresh `EHLO` gets `503`; relayed and
  fetched mail is tested against the address the relay observed; mail is refused with a temporary error below
  100 MB free (`MinimumFreeDiskSpaceMB`); serious errors also go to the Windows Event Log; existing out-of-office
  replies honour the RFC 3834 suppressions; old `hm_imapexpunged` rows are pruned once at first start.
- Early-bound COM clients compiled against a 6.2.22 pre-release interop assembly must be recompiled; late-bound
  scripts and anything compiled against 6.2.21 are unaffected.

**Known limitations:** statement timeouts have been exercised only on SQL Server Compact; the weekly C++ CodeQL
analysis was not running; SCRAM-SHA-1 is not offered (GitHub issue 49).

Full notes: [the 6.2.24 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.24).

## [6.2.23-alpha2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2) - 2026-09-04 - pre-release

A small, deliberate pre-release: one reported defect fixed, seven fixes taken from the original project, which had
become active again, and hardening that had been written down as missing.

**Fixed**

- Thunderbird's Sent copies were refused once `UTF8=ACCEPT` was enabled (GitHub issue 53): the RFC 6855
  `UTF8 (~{n}` wrapper on `APPEND` is now recognised.
- From upstream: the IP-based spam tests of relayed and fetched mail ran against the HELO literal the client
  chose rather than the address the receiving server observed; they use the observed address now.
- From upstream: `RSET` before `EHLO`, or STARTTLS without a fresh `EHLO`, opened a transaction with no greeting.
  A `MAIL FROM` straight after the TLS handshake now gets `503 Bad sequence of commands`, as RFC 3207 requires.
- A reverse-DNS answer with a trailing dot no longer empties the PTR result; null dereferences in the script host
  and the IMAP notification client.

**Hardening**

- Control Flow Guard on `hMailServer.exe` (GitHub issue 45); the first SSH-signed release tag (GitHub issue 46);
  an unattended install whose database step fails now fails; `IMAPFolder.Delete` reports a refused deletion.

**Repository**

- `hmailserver/docs/RegressionEnvironment.md` (GitHub issue 48), a DCO check on pull requests (GitHub issue 47),
  the C# tree `dotnet format` clean (GitHub issue 44), and SCRAM-SHA-1 closed as not planned (GitHub issue 49).

**Schema:** 6025, unchanged. Everything in Alpha 1's notes still applies to an installation coming from 6.2.21.

Full notes: [the 6.2.23 Alpha 2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha2).

## [6.2.23-alpha1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1) - 2026-08-21 - pre-release

A large feature release cut as an alpha, not for production: 83 commits since 6.2.22-pre6. It also fixes one path
that was silently losing mail.

**New**

- Shared and delegated IMAP mailboxes under `#Users.owner@domain.folder`, reachable when the owner's folder
  carries an RFC 4314 ACL granting the caller rights.
- A full-text search index that narrows `SEARCH BODY` and `TEXT` without changing results
  (`IndexerFullText=1` with message indexing, both off).
- RFC 3464 machine-readable bounces; BINARYMIME (RFC 3030), with relay of a binary message refused rather than
  converted.
- PROXY protocol v1 and v2 and Postfix `XCLIENT`, trusted only from listed peers; an external HTTP filtering hook
  (`FilterHookUrl`) for an engine such as rspamd.
- A sender blacklist; per-account spam thresholds; a domain-wide out-of-office reply; distribution-list
  moderation and a per-list bounce address.
- Windows Event Log integration; OTLP metrics and logs; W3C trace context ingestion; IPv6 on the REST, metrics
  and web-services listeners; five REST routes and `GET /api/v1/openapi.json`; DMARC aggregate reports in the
  RFC 9990 form behind `DmarcRptSchemaVersion=2`.
- Two COM methods for data-subject requests, `Account.ExportMessages` and `Utilities.EraseAddressTraces`, and
  `docs/WarmStandby.md`.

**Fixed**

- Local delivery could lose a message silently when the account-level copy could not be written; the sender is
  now told.
- Two database backends could wait forever (`DatabaseStatementTimeout`); an unattended upgrade could hang at an
  invisible password dialog; the white-list cache never cached.

**Schema:** 6022 to 6025 (6023 the full-text index tables, 6024 the domain out-of-office columns and
`hm_blocked_senders`, 6025 `hm_messages.messageflags` widened, per-account spam settings and list moderation).

**Upgrading**

- From 6.2.21 the schema moves from 6011 to 6025 in fourteen steps. The upgrade is one-way; take a database and
  data-directory backup first. The 6024 to 6025 step rewrites the largest table.
- New defaults: mail refused below 100 MB free, errors written to the Windows Application log, statement timeouts
  of 30 seconds on PostgreSQL and MySQL, QRESYNC tombstones pruned to 5,000 per mailbox, existing out-of-office
  replies suppressed for lists and automatic mail, bounces as `multipart/report`, and `PreferredHashAlgorithm`
  below SHA-256 refused.
- With IMAP ACL enforcement on, which is the default, any account holder whose client speaks `SETACL` can
  grant another local account access to their own folders, with no administrator step.
- A COM vtable layout was repaired to match 6.2.21; clients early-bound against a 6.2.22 pre-release must be
  recompiled, and a hand-copied install must re-register with elevation.

Full notes: [the 6.2.23 Alpha 1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.23-alpha1).

## [6.2.22-pre6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6) - 2026-08-20 - pre-release

A pre-alpha. The same build as pre5 (build 30, commit `bf0d38a`), republished with its SBOMs and a cosign bundle
for every asset, which pre5 could not carry. Its theme is standards catching up with reality.

**DMARC and algorithms**

- DMARCbis: the DNS tree walk (RFC 9989 section 4.10) decides organisational domains, with the Public Suffix List
  as the `DmarcTreeWalkEnabled=0` escape hatch; the `np=` tag for subdomains that do not exist; aggregate reports
  name the DKIM selector.
- DKIM `rsa-sha1` is refused and a domain configured to sign with it signs `rsa-sha256` (`DkimAcceptSha1=1`
  restores both); keys below 1,024 bits are refused; SPF void lookups are capped at two (`SpfVoidLookupLimit`).

**Certificates**

- The ACME renewal window is two thirds of the certificate's lifetime, ready for shorter lifetimes, and the CA's
  ARI window (RFC 9773) is consulted.

**Mail flow and operations**

- A full mailbox is refused at `RCPT` with `452 4.2.2` (`RejectFullMailboxAtRcpt=0` restores the old way); quota
  warnings at `QuotaWarningPercent` (90) (schema 6022).
- A relay host per sending domain (schema 6021; GitHub discussion 31).
- A queryable message trace (schema 6020, off); archive retention (`ArchiveRetentionDays`, 0); per-domain metric
  labels (`MetricsPerDomainEnabled`, off).

**Schema:** 6019 to 6022.

**Upgrading:** from pre3, DBUpdater moves the schema from 6019 to 6022. Not for a production server.

Full notes: [the 6.2.22-pre6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre6).

## [6.2.22-pre5](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre5) - 2026-08-20 (the tag's date) - tag only

Superseded by 6.2.22-pre6, which is the same commit: pre5 went out carrying only the installer, its release was
deleted, and the tag remains.

## [6.2.22-pre4](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.2.22-pre4) - 2026-08-20 (the tag's date) - tag only

Withdrawn: its installer could not create its own database on SQL Server Compact. The release was deleted and
the tag remains; do not build from it. The 6.2.22-pre6 notes explain both.

## [6.2.22-pre3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3) - 2026-08-19 - pre-release

The third pre-release of 6.2.22, whose theme is authentication: per-account two-factor authentication, and the
chain it needed first.

**Authentication**

- App passwords (schema 6016): a per-account credential, revocable on its own, for IMAP, POP3 and SMTP.
- Per-account TOTP (schema 6017): once enrolled, the account password stops being a mailbox credential and an
  app password becomes the only one.
- A password policy the server enforces where a password is chosen; password expiry and history (schema 6019).
  The server had no self-service password change then, so an expired password was reset by an administrator.
- ES256 OAuth2 tokens verify.

**Anti-spam, IMAP and POP3**

- An administrator-reviewable quarantine (schema 6018), off by default.
- `FETCH BINARY` on a composite section, a known limitation of pre2, is fixed; POP3 `EXPIRE` and `LOGIN-DELAY`
  (RFC 2449).

**Schema:** 6015 to 6019. **Upgrading:** from pre2, DBUpdater runs the four steps.

Full notes: [the 6.2.22-pre3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre3).

## [6.2.22-pre2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2) - 2026-08-18 - pre-release

The second pre-release of 6.2.22: the server now sends the reports it has always asked others for.

**Reporting**

- DMARC aggregate reports (RFC 7489 `rua=`), inert until `DmarcRptFromAddress` is set, with the external
  destination check enforced; no forensic reports.
- TLS-RPT reporting made verifiable with `Utilities.SendTlsRptReports`.

**Authentication and anti-virus**

- Per-account lockout (`AccountLockoutThreshold`, off), which a distributed attack cannot spread around.
- POP3 `AUTH-RESP-CODE` (RFC 3206), `PIPELINING` and `IMPLEMENTATION` (RFC 2449).
- `AVFailAction`: a message that could not be scanned may be held and returned instead of delivered.

**IMAP**

- `BINARY` (RFC 3516) and `OBJECTID` (RFC 8474, schema 6015).

**Schema:** 6014 to 6015. **Known limitation:** `FETCH BINARY` on a composite section returned an empty literal
(fixed in pre3).

Full notes: [the 6.2.22-pre2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre2).

## [6.2.22-pre1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1) - 2026-08-16 - pre-release

The first pre-release of 6.2.22: a Sieve wave, the Microsoft 365 OAuth switch-over, IMAP extensions, and two
reported bugs fixed. (The notes speak of two schema steps; from 6.2.21 there are three, 6012 to 6014.)

**Fixed**

- The built-in ACME client had never worked against a real Let's Encrypt (GitHub issue 34): the challenge
  locator missed the pretty-printed JSON. Failure messages now name the domain and quote the CA's response.
- With *Keep log files open*, log lines could stay invisible for forty minutes (GitHub issue 33); every line is
  now handed to the operating system as it is written.

**New**

- Microsoft 365 XOAUTH2 for outbound relay and POP3 collection, ahead of Basic authentication being switched
  off for SMTP relay.
- Sieve: variables, editheader, duplicate, reject and ereject, include, enotify, date and currentdate, spamtest,
  ihave and environment, mailboxexists and `:regex`; ManageSieve response codes and `RENAMESCRIPT`.
- Ten IMAP extensions - APPENDLIMIT, LITERAL-, LIST-STATUS, UNAUTHENTICATE, QUOTA, PREVIEW, MULTIAPPEND,
  REPLACE, SAVEDATE (schema 6013) and METADATA (schema 6014) - and SIZE on the SMTP client, which now opens every
  delivery with `EHLO`.
- An auto-reply start date and rule criteria up to 2,000 characters (schema 6012).

**Schema:** 6011 to 6014. **Upgrading:** databases upgrade automatically; take a backup before installing a
pre-release.

Full notes: [the 6.2.22-pre1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.22-pre1).

## [6.2.21](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21) - 2026-08-15

The fix for inbound mail from a Postfix relay that could hang for ever, found by running a real Postfix.

**SMTP**

- GitHub discussion 18: Postfix sends the end of the body, the terminating dot and `QUIT` in one segment, and
  every end-of-data check had looked only at the end of the buffer. The standard terminator is now found
  anywhere, and what follows it is handed to the command parser; bare-LF terminators are still recognised only
  at the very end, the CVE-2023-51764 smuggling rule. The Postfix test rig is in `hmailserver/test/PostfixBench`.
- Dot transparency across buffer boundaries, in both directions, which could silently drop a dot or truncate a
  message.
- A malformed DNS answer is retried once over TCP.

**Sieve**

- Sieve scripts move with domain and account renames and are removed with deletes, so a recreated address no
  longer inherits the previous holder's filter; the Control Panel writes a script only after the save succeeds.
- The `body` test (RFC 5173) with `:text`, `:content` and `:raw`.

**IMAP**

- `SORT` with an empty criteria list spun a connection thread for ever; it is rejected. The message cache could
  never evict and only counted upwards; fixed.

**Control Panel**

- The MX query tool asks the server's own resolver through `Utilities.ResolveMXRecords` (GitHub issue 29).
- Seven things the retired Administrator could do are restored, among them pausing the server and renaming a
  domain or an account; the Welcome page's icons are centred (GitHub issue 30, reported by `@helptiger`).

**Schema:** 6011, unchanged.

**Known issues:** equal-preference MX records are not randomised; one-click unsubscribe (RFC 8058) is not
implemented.

Full notes: [the 6.2.21 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.21).

## [6.2.20](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20) - 2026-08-15

The fix for fresh installations of 6.2.19, which failed or hung. Upgrades of existing installations were never
affected.

**Fixed**

- Database creation failed: statements added to the create scripts were separated by a single newline, and SQL
  Server Compact executes one statement per command. The create scripts of all backends are fixed.
- A silent install hung on a hidden dialog; every installer dialog now takes its default button under
  `/SUPPRESSMSGBOXES`, and the installer exits with a code a script can read.

**New**

- IMAP `THREAD` (RFC 5256) with `ORDEREDSUBJECT` and `REFERENCES`.

**Schema:** 6011, unchanged.

Full notes: [the 6.2.20 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.20).

## [6.2.19](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19) - 2026-08-15

**Fresh installations of 6.2.19 fail; use 6.2.20.** Upgrades are unaffected. The release fixes the custom DNS
server that 6.2.16 to 6.2.18 broke, and adds directory provisioning.

**Fixed**

- With a custom DNS server configured, 6.2.16, 6.2.17 and 6.2.18 could resolve nothing through it (GitHub issue
  25): MX, DNSBL, SPF and SURBL lookups all failed. Pinned by a regression test.
- A UID could be handed out twice; a colon in an account address is refused; a session could outlive its
  `io_context`; a status card could report failing backups on a healthy server; a certificate handle leaked.

**New**

- An LDAP directory as an account source (`Settings.PreviewDirectorySync`, `ApplyDirectorySync`), with a Control
  Panel page: opt-in per domain, and nothing is ever deleted.
- Sieve `imap4flags` now reaches the stored message.
- The `[Settings]` values move into the database so they can be administered remotely, with the file still
  winning where both carry a value.

**Schema:** 6005 to 6011.

**Known issue:** GitHub issue 26, a partial `FETCH BODY[]<offset.length>`, does not reproduce here.

Full notes: [the 6.2.19 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.19).

## [6.2.18](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18) - 2026-08-12

Finishes on the delivery side what 6.2.17 began, and stops a database outage bouncing mail.

**Fixed**

- A recipient lookup that fails because the database did not answer now returns `451` rather than `550`, so a
  database briefly locked by a backup defers mail instead of bouncing it. `DBConnectionAcquireTimeout` now
  defaults to 60 seconds.
- Bounded waits (GitHub issues 23 and 24): an absolute ceiling on outbound sessions (`ClientSessionCeiling`, 30
  minutes); ClamAV on the delivery path; `DNSQueryTimeout` (10 s), `ScriptTimeout` (60 s) and
  `ExternalProcessTimeout` (300 s); pre-authentication IMAP buffering capped at 11 MB; a backup restore validates
  the archive before deleting anything.
- The first `/analyze` pass: buffers sized in bytes where characters were counted, an overrun on long paths,
  and a log rotation that tested an uninitialised value.

**Documentation**

- `Roadmap.md` rewritten after a comparison with Postfix and Dovecot, Stalwart, iRedMail, Mail-in-a-Box and
  Exchange Online.

**Schema:** 6005, unchanged.

**Correction (12 August 2026):** ARC sealing is narrower than the notes first said: relayed third-party mail is
never sealed.

Full notes: [the 6.2.18 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.18).

## [6.2.17](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17) - 2026-08-12

Reproduces and bounds a stall after `354` on mail from a trusted incoming relay, with the stall now logged by
stage. (6.2.21 later found the defect GitHub discussion 18's reporter actually had.)

**Fixed**

- SpamAssassin's wait is bounded (`SAMaxTimeout` plus 30 seconds), after which the message is accepted without a
  verdict.
- `FinalizationTimeout` (240 seconds, 0 disables it) answers `451 4.3.1` rather than leaving the sender waiting.
- Acceptance is timed per stage and each spam test individually, and a slow stage is logged.

**Also**

- The README rewritten around what the server does; per-version history moved to the release pages.
- Regression coverage for clients that vanish mid-operation, and an installer smoke test on a clean machine.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.17 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.17).

## [6.2.16](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16) - 2026-08-11

A patch over 6.2.15 fixing one reported Control Panel defect. No server change.

**Fixed**

- Dismissing the Ctrl+K settings palette raised an error dialog (GitHub issue 21), on nearly every search in
  6.2.14 and 6.2.15.

**Schema:** 6005, unchanged. **Not fixed:** GitHub discussion 18, tracked in GitHub issue 20.

Full notes: [the 6.2.16 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.16).

## [6.2.15](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15) - 2026-08-11

IMAP sequence sets behave as RFC 3501 defines them, and every file the server writes has an owner.

**IMAP**

- `*` is resolved on either side of a colon and descending ranges are normalised in all four sequence-set
  parsers. Before, `UID STORE *:* +FLAGS (\Deleted)` flagged every message and `UID EXPUNGE *` deleted every
  deleted message.
- `STATUS ... (RECENT)` no longer reports the selected folder's count for every folder.

**Data safety**

- A failed restore could empty the data directory; the restore now confirms the replacement store exists before
  deleting anything.
- An unreadable message file no longer hangs an outbound connection; several paths no longer leave orphaned
  files; the account cache has a size limit.

**From the forum**

- `BOOST_USE_WINAPI_VERSION` raised to Windows 10; upstream's end-of-input rule adopted, with a truncated-`BDAT`
  defect it exposed fixed before release; DKIM header-name case under `simple` canonicalisation (upstream PR 530).

**Also**

- ManageSieve limits authentication attempts; the SMTP `DATA` path gains debug logging.
- Control Panel pages for settings that had none: Authentication, Administrative access, DNS resolver, Web
  services and autoconfiguration, message archiving, and the consistency scan's findings in Diagnostics.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.15 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.15).

## [6.2.14](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14) - 2026-08-11

Two community reports fixed, 21 defects found by an adversarial audit of the server fixed, and settings you can
search for.

**Reported**

- Backup would not start from the Control Panel (GitHub issue 19); the log-retention setting existed but could
  not be found (GitHub issue 16).

**Mail loss, corruption and crashes**

- A failed message copy crashed delivery; IMAP `APPEND` reported success for messages never written; string SQL
  parameters on MS SQL and SQL CE were bound from freed memory; a failed transaction start leaked a pooled
  connection; IMAP `CLOSE` expunged without telling other sessions; a refused POP3 session released another's
  lock.

**Security**

- DKIM test mode (`t=y`) no longer turns a failed signature into a pass; IMAP SASL credentials are no longer
  logged; every DKIM key record at a selector is examined; MTA-STS and MX failover hold for large recipient sets;
  REST requests are time-bounded; `ES256` tokens are refused with a clear message.

**Finding settings**

- Ctrl+K searches all 227 settings by label and key; settings moved to the pages where they are used; "Advanced
  hardening" became "Advanced INI settings".

**Schema:** 6005, unchanged.

Full notes: [the 6.2.14 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.14).

## [6.2.13](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13) - 2026-08-11

A community-feedback bug-fix release, hardening the SMTP, IMAP and SpamAssassin paths against malformed input.

**Fixed**

- A `DATA` stall when relaying from Postfix or Proxmox Mail Gateway, caused by a reverse-DNS lookup on the network
  I/O thread; `BDAT` refusals, pipelined data and a wedged connection handled.
- A malformed IMAP partial-fetch range could crash the server or read heap memory; `OnClientLogon` fires from
  every `AUTHENTICATE` mechanism.
- A malformed or empty `spamd` response could hang a session, corrupt headers or lose the message.
- DBUpdater labels versions 6002 to 6005; a failed database step reaches the installer as a visible error; a
  customised `EventHandlers.vbs` survives reinstall; Control Panel diagnostics read the right properties.

**Schema:** 6005, unchanged. **Upgrading:** in place over any earlier release; the minimum OS is Windows 10 1607
or Windows Server 2016, 64-bit.

Full notes: [the 6.2.13 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.13).

## [6.2.12](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12) - 2026-08-10

Every C# component in the product now targets .NET 8. No server-core changes.

**Changed**

- DBSetup, DBSetupQuick, DBUpdater, the Data Directory Synchronizer and the Shared library move from .NET
  Framework 4.8.1 to .NET 8, building against a checked-in COM interop wrapper.
- The VB6 migration wizards are replaced by an Import Tool: accounts from a text file, and messages from mbox files
  through the COM API, with the old tool's documented defects fixed.
- The .NET 8 Desktop Runtime is a server prerequisite and the supported floor is Windows 10 1607.
- CI builds every C# project; the regression suite runs on NUnit 4.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.12 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.12).

## [6.2.11](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11) - 2026-08-07

A Control Panel accessibility fix, and the regression suite run in full for the first time. No server-core
changes.

**Fixed**

- The alias list on the Domains page and the Live logs list announced their CLR type name to screen readers
  instead of their content.
- Run on the release machine for the first time, the ClamAV and SpamAssassin tests failed twice, both times on
  a defect in the test: the EICAR test now sends a real attachment, and the `spamd` stop is awaited.
- The README documents a working SpamAssassin setup on Windows and the suite's need for elevation.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.11 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.11).

## [6.2.10](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10) - 2026-08-07 (the tag's date)

A security and housekeeping release: an authorisation defect in the COM API, two unmaintained administration
front-ends removed, and the code-quality findings taken to zero.

**Security**

- Fifteen COM methods rejected an unauthorised caller with `return false`, which is `S_OK`, so each reported
  success for a call it had refused, and five handed back uninitialised memory. They return access denied now.
  Found by running CodeQL's C++ suite by hand.
- The PHP WebAdmin, which kept the administrator password in plaintext in a PHP session, is removed.

**Removals**

- The retired Administrator (`hMailAdmin.exe`) is gone from the repository; "Administrative tools" is renamed
  "Remote administration support (registers the COM API for scripts)", which is what it does.

**Quality**

- Code-quality findings in shipped, hand-written code from 958 to 0, with real defects fixed on the way.

**Schema:** 6005, unchanged.

**Upgrading:** a drop-in over 6.2.9. Anyone who ticked "Administrative tools" for the WebAdmin or the
Administrator should use the Control Panel, which connects to a remote server directly.

Full notes: [the 6.2.10 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.10).

## [6.2.9](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9) - 2026-08-07 (the tag's date)

A follow-up to 6.2.8. No server-core changes.

**Fixed**

- LiveCharts 2.0.5 painted the dashboard charts as a white block in the dark theme (GitHub issue 11); the chart
  background is now set explicitly.

**Quality and repository**

- The Control Panel has automated tests for the first time (GitHub PR 12): `ControlPanel.Core`,
  `ControlPanel.Tests` and `ControlPanel.sln`.
- Release tags protected, secret scanning on, a Code of Conduct added.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.9 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.9).

## [6.2.8](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8) - 2026-08-07 (the tag's date)

A Control Panel bug-fix release. Both defects made a healthy server look broken. No server-core changes.

**Fixed**

- Every data-driven list pane showed blank rows (GitHub issue 6), from domain aliases to account rules.
- The Control Panel died after a service restart with "The RPC server is unavailable" (GitHub issue 7); it now
  reconnects on its own, waits for the server to report Running, and no longer starts a service the
  administrator stopped.

**Dependencies**

- WPF-UI 4.3.0 and QRCoder 1.8.0 in the Control Panel; LiveCharts held at 2.0.0-rc2.

**Schema:** 6005 at the tag (the published notes said 6001, which the tree at the tag does not bear out); no
database change.

Full notes: [the 6.2.8 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.8).

## [6.2.7](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7) - 2026-06-15 (the tag's date)

A Control Panel usability release. No server-core changes since 6.2.6.

**Changed**

- A browse button on every field that holds a path.
- One-click DKIM: an RSA-2048 key pair generated with the exact DNS TXT record to publish.
- Password reveal, a generator for strong passwords, and the external POP3 password masked.
- A date picker for the auto-reply expiry, number boxes, Enter and Esc in every dialog, Copy on MX query and
  Diagnostics output, and a window that remembers its size and position.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.7 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.7).

## [6.2.6](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6) - 2026-06-15 (the tag's date)

A Control Panel and installer polish release, with one new opt-in protocol mode.

**Changed**

- IMAP4rev2 (RFC 9051) as an opt-in session mode through `ENABLE IMAP4rev2`; IMAP4rev1 behaviour is unchanged
  until a client asks.
- A Control Panel visual redesign on a theme-aware colour-token system, and a Welcome page of quick actions.
- Every configurable server setting has a Control Panel control.
- Two-factor setup shows a scannable QR code.
- The installer's database page scales with DPI, and the dead legacy dependency installers are gone.

**Schema:** 6005, unchanged.

Full notes: [the 6.2.6 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.6).

## [6.2.5](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5) - 2026-06-15 (the tag's date)

A critical fix release: the default fresh install, SQL Server Compact with DPAPI-protected secrets, could not
connect to its own database. Anyone running a default install should upgrade.

**Fixed**

- The DPAPI-protected database password was truncated by a 255-character INI read buffer, giving SQL CE error
  25028; the buffer is now 4,096 characters.
- The create scripts stamped schema 6004 while the server required 6005, so a new install reported its database
  as too old.

**Control Panel**

- Full settings parity with the classic Administrator, with the Server Status page and the per-account rule
  editor.

**Schema:** 6005.

Full notes: [the 6.2.5 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.5).

## [6.2.4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4) - 2026-06-15 (the tag's date)

A Control Panel release that completes the last functional-parity item from the classic Administrator. The
server, the Control Panel and the installer all report 6.2.4.

**Changed**

- Active Directory account pickers: a read-only browser of the forest's domains and users, for an account's
  directory link and for distribution-list recipients.
- "Settings could not be read" now names the real cause.
- The MariaDB Connector/C client is staged into the build output, so MySQL and MariaDB back ends work out of the
  box.

**Schema:** 6005.

Full notes: [the 6.2.4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.4).

## [6.2.3](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3) - 2026-06-15 (the tag's date)

The first server-focused release since 6.2.2. Every new behaviour is additive and off by default. The installer
no longer ships the classic Administrator; the Control Panel is the only bundled GUI.

**Mail filtering**

- A Sieve (RFC 5228) interpreter run at local delivery, and a ManageSieve (RFC 5804) listener behind
  `ManageSieveServerPort`.

**Operability**

- `/livez`, `/readyz` and `/healthz` on the metrics listener; Prometheus metrics for the database, TLS, the queue,
  authentication, delivery outcomes and command latency.
- A slow-query log with secrets redacted (`SlowQueryLogMilliseconds`); a graceful shutdown drain
  (`ShutdownDrainSeconds`); `MessageStoreFsync`; a read-only store consistency check
  (`MessageStoreConsistencyCheck`); log retention (`LogDeleteDays`); the session id in the `Received` header;
  an active/passive runbook.

**SMTP**

- BATV backscatter protection (`BATVEnabled`, `BATVSecret`); CHUNKING and `BDAT` (RFC 3030) on receipt.

**Supply chain**

- SBOMs for every release, Dependabot and a dependency-review gate.

**Schema:** 6005.

Full notes: [the 6.2.3 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.3).

## [6.2.2](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2) - 2026-06-14 (the tag's date)

The 6.2.2 server with a substantially upgraded Control Panel.

**Server**

- Full RFC 4013 SASLprep for non-ASCII SASL credentials; RS256 OAuth2 bearer tokens covered by regression tests.

**Control Panel**

- Search on the long list pages, confirmation before deletes, numeric validation, the OS theme followed by default,
  and the Advanced group split into Security, Network and Maintenance.
- Anti-virus test buttons and scanner presets, event-script templates, and an "Advanced hardening" page for
  settings that had no control.
- Screen-reader names and stable automation ids throughout.

**Schema:** 6005.

Full notes: [the 6.2.2 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.2).

## [6.2.1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1) - 2026-06-14 (the tag's date)

Deliverability and modern SMTP standards. Every new feature is off or backward compatible.

**New**

- `PIPELINING` (RFC 2920), `SMTPUTF8` (RFC 6531 and 6532), `ENHANCEDSTATUSCODES` (RFC 2034) and DSN (RFC 3461 and
  3464).
- SRS for forwarded mail (`SRSEnabled`, `SRSSecret`); per-IP and per-destination rate shaping
  (`MaxSubmissionsPerIPPerMinute`, `MaxOutboundPerDestinationPerMinute`).

**Fixed**

- A trailing NUL from `Unicode::WideToMultiByte` could corrupt DPAPI-protected stored secrets.

**Schema:** 6004 to 6005.

Full notes: [the 6.2.1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.1).

## [6.2.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0) - 2026-06-14 (the tag's date)

The hMailServer Control Panel arrives: a new .NET 8 WPF administration app beside the full 6.x server. The
.NET 8 Desktop Runtime is bundled and installed when missing.

**New**

- The Control Panel: a live dashboard, domains and accounts, the delivery queue with live logs, a graphical rules
  editor, every settings area including DANE, MTA-STS, ARC, TLS-RPT, ACME and the REST API, and the utilities.
- The classic Administrator and the Web Control Deck remain included.

**Schema:** 6004.

Full notes: [the 6.2.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.2.0).

## [6.1.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0) - 2026-06-12 (the tag's date)

A UI and UX modernisation across the desktop Administrator, a new web interface and the installer. The server core
is unchanged from 6.0.0 build 4 apart from the new web-page endpoint.

**New**

- The Web Control Deck, served by the REST API listener (`RestApiPort`): a live dashboard, domain and account
  management, queue retry and delete, and DANE TLSA records to publish.
- The Administrator gains light and dark themes, a Ctrl+K command palette and a modernised dashboard.
- The installer's wizard is rebranded and ships the WebAdmin payload.

**Schema:** 6001.

Full notes: [the 6.1.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.1.0).

## [6.0.0-B4](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4) - 2026-06-12 (the tag's date) - pre-release

Build 4, which fixes the service crash at startup that build 3 caused on fresh installations.

**Fixed**

- Build 3 bundled VS2019-era VC++ runtime DLLs, which the service loaded in place of the current ones; the
  installer now ships `Microsoft.VC145.CRT` 14.51 and no longer bundles the obsolete UCRT.
- A stdout and stderr pipe deadlock in `build/run-tests.ps1`.

**Schema:** 6001.

Full notes: [the 6.0.0-B4 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B4).

## [6.0.0-B3](https://gitlab.com/Progressiverobot/hmailserver/-/tags/v6.0.0-B3) - 2026-06-12 (the tag's date) - tag only

Build 3 exists as a tag only; nothing was saved of a release for it, and there is no evidence one was published.

## [6.0.0](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0) - 2026-06-12 (the tag's date)

The production release of hMailServer 6.0, 64-bit only. **The `v6.0.0` tag points at build 2 (`e4b716318`), while
the published release shipped the build-4 installer, `hMailServer-6.0.0-B4-x64.exe`, and its notes describe build
4.**

**Fixed in builds 3 and 4**

- Build 4: the service no longer crashes at startup on fresh installations from mismatched VC++ runtime DLLs.
- Build 3: MySQL and MariaDB database creation, and `libmysql.dll` bundled.

**Highlights of 6.0**

- OpenSSL 4 with TLS 1.2 and 1.3, MTA-STS, DANE with in-process DNSSEC validation, ARC sealing, Ed25519 DKIM, ACME
  (Let's Encrypt) built in, the REST admin API, Prometheus metrics, IMAP `MOVE`, `ID` and `SPECIAL-USE`, PBKDF2
  password hashing, and the Visual Studio 2026 toolchain.

**Schema:** 6001 (DBUpdater upgrades a 5.7 database).

Full notes: [the 6.0.0 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0).

## [6.0.0-B1](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1) - 2026-06-12 (approximately, when its installer was uploaded) - pre-release

The first regression-validated build of the modernised 6.0.

**New in 6.0**

- MTA-STS and DANE with DNSSEC for outbound TLS, ARC sealing, TLS-RPT reporting, ACME with automatic apply, the
  REST admin API, MTA-STS, autoconfig and autodiscover hosting, Prometheus metrics, JSON logging, PBKDF2 password
  hashing with transparent rehash, Ed25519 DKIM, DMARC, IMAP `MOVE`, `ID` and `SPECIAL-USE`, 8BITMIME, an
  Administrator dashboard with TOTP two-factor sign-in, and TLS 1.2 and 1.3 defaults.
- Built with Visual Studio 2026 (v145), OpenSSL 4.0.1, Boost 1.91, PostgreSQL 18.3 libpq and .NET Framework 4.8.1.

**Fixed**

- A latent `CStdStr::FormatV` buffer-count bug that crashed the service.

**Schema:** 6001 (upgrade from 5.7 through DBUpdater).

**Licence:** AGPLv3, based on hMailServer by Martin Knafve.

Full notes: [the 6.0.0-B1 release](https://gitlab.com/Progressiverobot/hmailserver/-/releases/v6.0.0-B1).
