#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The two ends of a release's pipeline: what goes into it, and publishing it.

A v* tag's pipeline (.gitlab-ci.yml, the release stage; docs/ContinuousIntegration.md,
"The release") builds and tests, then waits for a maintainer to start the `release`
job. The release-packages job has run `gather` before it; that job runs `collect`;
Authenticode signs the installer, `release-sbom` runs `sbom`, Sigstore signs every
file (build/ci/release-sign.sh), release-verify runs `check-provenance`, and
`release-publish`, also started by hand, runs `publish`:

  gather    (release-packages, .gitlab/ci/release-packages.yml) every file a release
            carries from the package jobs besides the generic packages - each
            distribution release's .deb and .rpm, the two AppImages and the source
            of the libraries linked into their runtime - and the source tarball,
            from the artifacts of the jobs that made them, each held to the SHA-256
            its job wrote (the tarball to build/check-source-tarball.py and this
            commit), copied under its name with its job's sums, and each but the
            runtime's sources, which is source and has none, given its SBOM from
            what its build recorded it was built against. release needs this one
            job for the forty-two it waits for, forty of them jobs release does not
            need already: a job may need fifty at most (GitLab's
            ci_needs_size_limit).

  collect   the tag is an annotated tag, it still points at the commit this
            pipeline built, and that commit is on master; it is signed by the one
            key .gitlab/allowed_signers lists as master has it - never as the tag's
            own tree has it, which a tag on a commit of its own could write - and
            the job refuses while that list holds more than one key, since release
            tags are the owner's alone (GOVERNANCE.md, 26 September 2026);
            Version.h says the version the tag names, so every file name made from
            it is the name a server looks for; the cosign image is gcr.io's pinned
            by digest, whatever a CI/CD variable made it, and the Azure settings the Authenticode job reads are all there; the
            security scans of the commit finished, and every finding they raised that
            SECURITY.md's policy counts is in a scan's baseline or accepted in
            .gitlab/security-exceptions.yml (build/ci/security-gate.py); and the
            artefacts of the jobs it needs - the Windows installer; the x86-64 .deb
            and .rpm, linux-build's; the AArch64 .deb and .rpm, from the package
            jobs deb-ubuntu26.04-arm64 and rpm-fedora44-aarch64, with their SBOM
            jobs' SBOMs; what release-packages gathered; the Helm chart,
            helm-chart-package's; the two SBOMs of the tree - are gathered into one
            directory under the names a release carries, each package, AppImage and
            the chart held to the SHA-256 its build job wrote and each SBOM to the
            file it describes, and the sums of every Linux file and the chart
            written as one file. The package hmailserver/<version> must not hold
            any of them with other bytes already, nor the installer, its SBOM or a
            bundle at all: those are made by the jobs after this one, and a second
            signing never makes the same bytes. Last, before anything is signed,
            what GitLab and Pages will take of the release is counted
            (check_budget). Nothing leaves the pipeline.

  sbom      the installer's own SBOM (build/asset-sbom.py), made from the bytes
            Authenticode signed - every compiled file a release carries goes out
            with an SBOM of its own, <file>.cdx.json (OpenSSF Baseline
            OSPS-QA-02.02; SECURITY.md, "Software bills of materials") - and the
            statement of the release's SLSA v1 build provenance, which names every
            file by its SHA-256, for release-sign to attest.

  check-provenance
            (release-verify) each primary file's .sigstore.json is its bundle, and
            the provenance names every file of the release and nothing else, built
            by this pipeline from this commit.

  publish   every signed file, its <file>.cosign.bundle, each primary file's
            <file>.sigstore.json and the provenance uploaded to the generic
            package hmailserver/<version> of this project, read back from the
            registry and held to the SHA-256 it was signed with - nothing is
            uploaded while the package holds a file of that name with other bytes,
            and a file it holds with the same bytes, from an earlier run of the job,
            is not uploaded again - once every compiled file's SBOM, every copy and
            the provenance have been held to the files, and every link and the site
            to what GitLab and Pages take - then the GitLab
            release created with one link per file, named after the file, which is
            what build/generate-update-feed.py and every server require. The notes
            are the version's section of CHANGELOG.md and the verification the
            release-verify job wrote. Last, the feed is rendered from the API as the
            pages job will render it, and the new release must be in it with its
            installer and bundle for Windows servers, for Debian and RPM servers on
            x86-64 and on AArch64, and for the AppImages.

A rehearsal (--rehearse; .gitlab/ci/release-rehearsal.yml) runs collect and sbom on a
protected batch* branch: the version is Version.h's, the installer
windows-packages-x64's, and the tag, the Authenticode settings, the scans and the
registry are not asked about. What it collects is signed as the branch, which no
server trusts, and published nowhere; publish and gather refuse --rehearse.

None asks for a secret: the pipeline's own CI_JOB_TOKEN reads the registry, uploads
and creates the release, and it is sent as a header and never printed. A release that exists already
is refused - a tag is spent once - and so is any project but this one.

Usage, from the repository root:
  python3 build/ci/release-chain.py gather [--out release-packages]
  python3 build/ci/release-chain.py collect [--rehearse] [--out release-assets]
  python3 build/ci/release-chain.py sbom [--rehearse] [--out release-sbom]
  python3 build/ci/release-chain.py check-provenance [--rehearse] [--assets release-signed]
  python3 build/ci/release-chain.py publish [--assets release-signed] [--verification release-verified/verification.md]
  python3 build/ci/release-chain.py --self-test      no network, no git, no token
"""

import argparse
import base64
import datetime
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def _load_feed():
    path = os.path.join(ROOT, "build", "generate-update-feed.py")
    spec = importlib.util.spec_from_file_location("generate_update_feed", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


feed = _load_feed()


def _load_asset_sbom():
    path = os.path.join(ROOT, "build", "asset-sbom.py")
    spec = importlib.util.spec_from_file_location("asset_sbom", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


asset_sbom = _load_asset_sbom()


def _load_gate():
    path = os.path.join(ROOT, "build", "ci", "security-gate.py")
    spec = importlib.util.spec_from_file_location("security_gate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The security scans' verdict (build/ci/security-gate.py), which collect asks for
# before anything is signed.
security_gate = _load_gate()


def _load_repos():
    path = os.path.join(ROOT, "build", "generate-package-repos.py")
    spec = importlib.util.spec_from_file_location("generate_package_repos", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The apt and dnf repositories the pages job adds to the feed's site, whose
# _redirects rules count with the feed's (check_budget).
repos = _load_repos()


def _load_certificate():
    path = os.path.join(ROOT, "build", "ci", "sigstore-certificate.py")
    spec = importlib.util.spec_from_file_location("sigstore_certificate", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# A Fulcio certificate's fields, read from its extensions and never by a search of its
# bytes (build/ci/sigstore-certificate.py): the provenance's certificate is held to the
# identity by its subject alternative name.
sigstore_certificate = _load_certificate()

PROJECT_ID = feed.PROJECT_ID
PACKAGE = "hmailserver"
API_BASE = "https://gitlab.com/api/v4"
# The identity Fulcio writes for this project's pipeline of tag <tag>, which
# SigstoreVerifier.cpp's GITLAB_IDENTITY_PREFIX accepts. build/check-release-chain.py
# holds the two to each other.
SIGNER_IDENTITY_PREFIX = "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/tags/"
SIGNER_ISSUER = "https://gitlab.com"
VERSION_H = os.path.join("hmailserver", "source", "Server", "Common", "Application", "Version.h")
ALLOWED_SIGNERS = ".gitlab/allowed_signers"
# The tag check reads the allow-list from here, fetched by name, and requires the
# tagged commit to be on it: master is protected, and a tag is not. The tagged
# tree's own .gitlab/allowed_signers is never read, since a tag on a commit that
# is not master's could carry a key of its own in it.
TRUSTED_BRANCH = "master"
TRUSTED_REF = "refs/remotes/origin/" + TRUSTED_BRANCH
# What git verify-tag prints for a good SSH signature: the principal and the
# fingerprint of the key that made it.
GOOD_SIGNATURE = re.compile(r'Good "git" signature for (\S+) with (\S+) key (SHA256:[A-Za-z0-9+/]+)')
CHANGELOG = "CHANGELOG.md"
# The settings release-authenticode reads (build/ci/windows-authenticode.ps1). Names
# only are ever printed.
AZURE_SETTINGS = ["AZURE_TENANT_ID", "AZURE_CLIENT_ID", "ARTIFACT_SIGNING_ENDPOINT",
                  "ARTIFACT_SIGNING_ACCOUNT", "ARTIFACT_SIGNING_PROFILE"]
# cosign's own image from gcr.io, pinned by digest: the rule build/check-release-chain.py
# holds the CI file's HM_COSIGN_IMAGE to (its PINNED_COSIGN, which it holds this to). The
# value is checked again here because a project or pipeline CI/CD variable of the same
# name replaces the file's at run time, and release-sign runs in whatever it names.
PINNED_IMAGE = re.compile(r"^gcr\.io/projectsigstore/cosign:[A-Za-z0-9._\-]+@sha256:[0-9a-f]{64}$")


class ReleaseError(Exception):
    pass


QUIET = []


def say(line):
    if not QUIET:
        print(line)


# ---------------------------------------------------------------- what a release carries

def version_of(tag):
    """The version a tag names: v6.3.5 -> 6.3.5. Refuses what is not a server tag."""
    if not tag or not feed.SERVER_TAG.match(tag):
        raise ReleaseError("%r is not a release tag: a release's tag is v, a digit, then digits, dots and a suffix "
                           "if it is a pre-release (v6.3.5, v6.3.5-rc1)" % tag)
    return tag[1:]


def sums_name(version):
    return "hmailserver-linux-%s-SHA256SUMS.txt" % version


def sbom_name(name):
    """The SBOM of one compiled file: <file>.cdx.json, beside it (build/asset-sbom.py)."""
    return name + asset_sbom.SUFFIX


# Every file a release carries is signed beside it as <file>.cosign.bundle, the name a
# server looks for. Each primary file's bundle goes out a second time, byte for byte,
# as <file>.sigstore.json (Roadmap3 row 25): the name Sigstore's own clients and
# OpenSSF Scorecard's Signed-Releases check look for. A server reads the first alone.
BUNDLE = ".cosign.bundle"
BUNDLE_COPY = ".sigstore.json"


def provenance_name(version):
    """The release's SLSA v1 build provenance (Roadmap3 row 26): an in-toto statement
    naming every file of expected_assets by its SHA-256, signed as the pipeline with
    cosign attest-blob - a Sigstore bundle holding a DSSE envelope. The bundle is the
    file; it has no bundle of its own, and is in no sums."""
    return "hmailserver-%s.intoto%s" % (version, BUNDLE_COPY)


def statement_name(version):
    """The provenance's statement as release-sbom writes it, unsigned, for release-sign
    to attest: it is not a file of the release (the bundle carries it)."""
    return "hmailserver-%s.intoto.json" % version


def tarball_name(version):
    """The source tarball, build/ci/source-tarball.sh's (docs/SourceTarball.md)."""
    return "hmailserver-%s.tar.gz" % version


def chart_name(version):
    """The Helm chart, build/ci/helm-chart-package.sh's (docs/ContainerImage.md). Not
    compiled and with nothing of its own to list, so no SBOM."""
    return "hmailserver-chart-%s.tgz" % version


def runtime_sources_name(version):
    """The source of every library linked into the AppImages' runtime, exactly as
    Alpine built it, which the LGPL-2.1 of libfuse3 and the GPL of lzo ask to go with
    the binary (build/ci/packages/appimage-runtime-sources.sh; docs/PackagesAppImage.md,
    "Licences"). Every AppImage's usr/share/hmailserver/licenses/appimage-runtime.txt
    names it. Source, not compiled, so no SBOM: its README.txt names each library, its
    version, its licence and where each file came from."""
    return "hmailserver-%s-appimage-runtime-sources.tar.gz" % version


# The package jobs whose files a release carries from 6.3.6 besides the generic ones:
# one .deb per Debian and Ubuntu release and architecture (.gitlab/ci/packages-deb.yml)
# and one .rpm per RPM distribution release and architecture (packages-rpm.yml), each
# built with that release's own compiler and libraries and installed in a clean
# container of it; the two AppImages, and the source of the libraries linked into
# their runtime (packages-appimage.yml). With the source tarball they are gathered by
# the release-packages job (.gitlab/ci/release-packages.yml, gather below), which makes
# each compiled one's SBOM, and the tarball's, and needs each build and its install or
# test job: release itself may need fifty jobs at most (GitLab's ci_needs_size_limit),
# and these are forty-two, forty of them jobs release does not need already.
DEB_DISTROS = ("debian12", "debian13", "ubuntu24.04")
DEB_ARCHES = ("amd64", "arm64")
# (the package job's target, the release in the file's name: rpm-build.sh's disttag)
RPM_TARGETS = (("fedora44", "fc44"), ("fedora43", "fc43"), ("el10", "el10"), ("el9", "el9"),
               ("leap16", "suse.lp160"), ("tumbleweed", "suse.tw"))
RPM_ARCHES = ("x86_64", "aarch64")
APPIMAGE_ARCHES = ("x86_64", "aarch64")
# The release-packages job's artifact: every gathered file with its SBOM beside it, and
# under job-sums/ each build job's own SHA256SUMS as that job wrote it.
GATHERED = "release-packages"
JOB_SUMS = "job-sums"


def gathered_builds(version):
    """What release-packages gathers, in the order the release's sums list it: for
    each, its name, the job that makes it, the job that installs or runs it where
    there is one, where that job leaves it, the sums it lists it in and under which
    name, where the build recorded what it was built against, for its SBOM, and
    whether it has one (every file but the runtime's sources, which is source)."""
    builds = []
    for distro in DEB_DISTROS:
        for arch in DEB_ARCHES:
            where = "packages-deb/%s-%s" % (distro, arch)
            # deb-build.sh: CPack's DEB-DEFAULT name for Version <v>-1~<distro>.
            name = "hmailserver_%s-1~%s_%s.deb" % (version, distro, arch)
            builds.append({"name": name, "kind": "deb", "job": "deb-%s-%s" % (distro, arch),
                           "test": "deb-%s-%s-install" % (distro, arch), "path": where + "/" + name,
                           "sums": where + "/SHA256SUMS.txt", "listed": name, "record": where + "/built-against", "sbom": True})
    for target, disttag in RPM_TARGETS:
        for arch in RPM_ARCHES:
            where = "packages-rpm/%s-%s" % (target, arch)
            name = "hmailserver-%s-1.%s.%s.rpm" % (version, disttag, arch)
            builds.append({"name": name, "kind": "rpm", "job": "rpm-%s-%s" % (target, arch),
                           "test": "rpm-%s-%s-install" % (target, arch), "path": where + "/" + name,
                           "sums": where + "/SHA256SUMS.txt", "listed": name, "record": where + "/built-against", "sbom": True})
    for arch in APPIMAGE_ARCHES:
        name = feed.installer_names(version)["appimage-" + arch]
        builds.append({"name": name, "kind": "appimage", "job": "appimage-" + arch, "test": "appimage-test-" + arch,
                       "path": "appimage/" + name, "sums": "appimage/SHA256SUMS-%s.txt" % arch, "listed": name,
                       "record": "appimage/built-against-" + arch, "sbom": True})
    # The source of the libraries linked into the AppImages' runtime: its job makes it
    # twice from the runtime jobs' records and compares, and writes its sums; source,
    # so no SBOM (runtime_sources_name).
    name = runtime_sources_name(version)
    builds.append({"name": name, "kind": "runtime-sources", "job": "appimage-runtime-sources", "test": None,
                   "path": "appimage-runtime-sources/" + name, "sums": "appimage-runtime-sources/SHA256SUMS.txt",
                   "listed": name, "record": None, "sbom": False})
    # The tarball's own job makes it twice and compares; there are no sums of it, and
    # build/check-source-tarball.py --commit holds it to the pipeline's commit instead.
    name = tarball_name(version)
    builds.append({"name": name, "kind": "source", "job": "source-tarball", "test": None, "path": "source-tarball/" + name,
                   "sums": None, "listed": None, "record": None, "sbom": True})
    return builds


def compiled_assets(version):
    """The files a release carries that each go out with an SBOM of their own (OpenSSF
    Baseline OSPS-QA-02.02): the installer; the generic .deb and .rpm for x86-64 and
    for AArch64; each distribution release's .deb and .rpm; the two AppImages; and the
    source tarball, whose SBOM lists the third-party code it holds. The ARM64 Windows
    installer (windows-packages-arm64, hMailServer-<v>-arm64.exe) is not collected
    until it has run on ARM64 hardware (Roadmap3 row 4, W3.1): it would come here, after
    the x64 one, signed by its own Authenticode call, with release-sbom making its SBOM
    (asset-sbom.py's windows-arm64), and into SERVED_PLATFORMS."""
    names = feed.installer_names(version)
    return [names["windows"], names["deb-amd64"], names["rpm-x86_64"], names["deb-arm64"], names["rpm-aarch64"]] + [
        build["name"] for build in gathered_builds(version) if build["sbom"]]


def primary_assets(version):
    """Every file a release carries but the compiled files' SBOMs: each has a bundle
    and a .sigstore.json copy of it. Beside the compiled files, the source of the
    AppImages' runtime's libraries, the chart, the sums and the tree's two SBOMs."""
    return compiled_assets(version) + [build["name"] for build in gathered_builds(version) if not build["sbom"]] + [
        chart_name(version), sums_name(version), "hmailserver.spdx.json", "hmailserver.cyclonedx.json"]


def expected_assets(version):
    """Every file a release carries, each signed beside it: the primary files and each
    compiled file's SBOM (docs/ContinuousIntegration.md, "What a release carries")."""
    return primary_assets(version) + [sbom_name(n) for n in compiled_assets(version)]


def release_files(version):
    """Every file a release is: expected_assets, a bundle beside each, a .sigstore.json
    copy of each primary file's bundle, and the provenance. What release-publish
    uploads and links, and nothing else."""
    expected = expected_assets(version)
    return expected + [n + BUNDLE for n in expected] + [n + BUNDLE_COPY for n in primary_assets(version)] + [
        provenance_name(version)]


def sums_members(version):
    """The files whose lines follow linux-build's own two in the release's sums, in
    this order: the AArch64 .deb and .rpm, as from 6.3.6's first chain; then every
    gathered file; then the chart. The Windows installer is not among them: collect
    runs before Authenticode changes its bytes."""
    names = feed.installer_names(version)
    return [names["deb-arm64"], names["rpm-aarch64"]] + [b["name"] for b in gathered_builds(version)] + [chart_name(version)]


def made_after_collect(version):
    """What the jobs after collect make: the installer's signed bytes (Authenticode) and
    the installer's SBOM, which describes those bytes (release-sbom)."""
    installer = feed.installer_names(version)["windows"]
    return [installer, sbom_name(installer)]


def collected_assets(version):
    """What collect gathers: every file but the installer's SBOM, which release-sbom makes."""
    later = made_after_collect(version)[1:]
    return [name for name in expected_assets(version) if name not in later]


# Where the package jobs whose files a release carries leave them
# (.gitlab/ci/packages-deb.yml, packages-rpm.yml): deb-ubuntu26.04-arm64 builds the
# generic arm64 .deb in Ubuntu 26.04's own container, and rpm-fedora44-aarch64 the
# generic aarch64 .rpm in Fedora 44's, under generic/ beside the .fc44 copy of the
# same build. Each job's SBOM job writes <package>.cdx.json beside the package.
ARM64_DEB_DIR = os.path.join("packages-deb", "ubuntu26.04-arm64")
AARCH64_RPM_DIR = os.path.join("packages-rpm", "fedora44-aarch64")


# Where the Helm chart's job leaves it, with its SHA-256 (.gitlab/ci/helm-chart.yml).
CHART_DIR = "helm-chart"
# A rehearsal's installer: windows-packages-x64's (.gitlab/ci/packages-windows.yml),
# since windows-installer runs for a v* tag alone. On a branch it names the installer
# hMailServer-<version>-<first 8 of the commit>-x64.exe, so that no branch build is
# mistaken for a release, with its SHA-256 in SHA256SUMS beside it.
REHEARSAL_INSTALLER_DIR = "packages-windows/x64"


def rehearsal_installer(version, commit):
    return "hMailServer-%s-%s-x64.exe" % (version, (commit or "")[:8])


def local(relative):
    """A path of the sources below as this platform writes one."""
    return os.path.join(*relative.split("/"))


def sources(version, rehearsal=None):
    """Where collect finds each file: the artifacts of the jobs `release` needs, or in
    a rehearsal (rehearsal = the commit) the rehearsal job's. The sums file is
    linux-build's, to which collect adds a line for every other Linux file
    (release_sums). The gathered files, each with its SBOM where it has one, are
    release-packages'."""
    names = feed.installer_names(version)
    deb_arm64 = os.path.join(ARM64_DEB_DIR, names["deb-arm64"])
    rpm_aarch64 = os.path.join(AARCH64_RPM_DIR, "generic", names["rpm-aarch64"])
    installer = os.path.join("release-windows", names["windows"])
    if rehearsal is not None:
        installer = os.path.join(local(REHEARSAL_INSTALLER_DIR), rehearsal_installer(version, rehearsal))
    places = {
        names["windows"]: installer,
        names["deb-amd64"]: os.path.join("linux-build", "packages", names["deb-amd64"]),
        names["rpm-x86_64"]: os.path.join("linux-build", "packages", names["rpm-x86_64"]),
        names["deb-arm64"]: deb_arm64,
        names["rpm-aarch64"]: rpm_aarch64,
        sbom_name(names["deb-amd64"]): os.path.join("linux-build", "packages", sbom_name(names["deb-amd64"])),
        sbom_name(names["rpm-x86_64"]): os.path.join("linux-build", "packages", sbom_name(names["rpm-x86_64"])),
        sbom_name(names["deb-arm64"]): sbom_name(deb_arm64),
        sbom_name(names["rpm-aarch64"]): sbom_name(rpm_aarch64),
        sums_name(version): os.path.join("linux-build", "packages", "SHA256SUMS.txt"),
        "hmailserver.spdx.json": os.path.join("sbom", "hmailserver.spdx.json"),
        "hmailserver.cyclonedx.json": os.path.join("sbom", "hmailserver.cyclonedx.json"),
        chart_name(version): os.path.join(CHART_DIR, chart_name(version)),
    }
    for build in gathered_builds(version):
        places[build["name"]] = os.path.join(GATHERED, build["name"])
        if build["sbom"]:
            places[sbom_name(build["name"])] = os.path.join(GATHERED, sbom_name(build["name"]))
    return places


def package_job_sums(version, rehearsal=None):
    """{file: (its build job's sums, the name that file gives it)}: deb-build.sh names
    the .deb as it is, and rpm-build.sh the generic copy by its place under generic/;
    every gathered package and AppImage, and the runtime's sources, by its job's sums
    as release-packages copied them; the chart by helm-chart-package's; and in a
    rehearsal the installer by windows-packages-x64's, which names it with the
    commit."""
    names = feed.installer_names(version)
    sums = {
        names["deb-arm64"]: (os.path.join(ARM64_DEB_DIR, "SHA256SUMS.txt"), names["deb-arm64"]),
        names["rpm-aarch64"]: (os.path.join(AARCH64_RPM_DIR, "SHA256SUMS.txt"), "generic/" + names["rpm-aarch64"]),
        chart_name(version): (os.path.join(CHART_DIR, "SHA256SUMS.txt"), chart_name(version)),
    }
    for build in gathered_builds(version):
        if build["sums"]:
            sums[build["name"]] = (os.path.join(GATHERED, JOB_SUMS, build["job"] + ".txt"), build["listed"])
    if rehearsal is not None:
        sums[names["windows"]] = (os.path.join(local(REHEARSAL_INSTALLER_DIR), "SHA256SUMS"), rehearsal_installer(version, rehearsal))
    return sums


# The component list release-sbom makes the installer's SBOM from, which linux-sbom
# writes and the release job keeps in release-tools/ (build/ci/linux-sbom.sh).
DEPENDENCY_LIST = "native-dependencies-x64.json"


def sbom_problems(directory, version, names=None):
    """Each compiled file's SBOM in directory describes that very file, by its SHA-256,
    and is well formed; problems as sentences."""
    problems = []
    for name in names or compiled_assets(version):
        document = os.path.join(directory, sbom_name(name))
        asset = os.path.join(directory, name)
        if not os.path.isfile(document):
            problems.append("%s goes out without its SBOM, %s: every compiled file a release carries has one "
                            "(SECURITY.md, 'Software bills of materials')" % (name, sbom_name(name)))
            continue
        lines = []
        try:
            answer = asset_sbom.verify(document, asset, lines.append)
        except (OSError, asset_sbom.SbomError) as e:
            answer, lines = 1, ["PROBLEM: %s" % e]
        if answer != 0:
            problems.append("%s does not describe %s: %s" % (sbom_name(name), name,
                                                          "; ".join(l for l in lines if l.startswith("PROBLEM")) or "unreadable"))
    return problems


def is_primary(name):
    """Not a signature: neither a bundle nor a bundle's .sigstore.json copy (nor the
    provenance, which is a bundle too)."""
    return not name.endswith((BUNDLE, BUNDLE_COPY))


def permalink_name(name, version):
    """The release's permanent download name, as every release since 6.2.19 has
    had it: the version taken out of a package's name (hMailServer-x64.exe,
    hmailserver_amd64.deb, hmailserver.x86_64.rpm; a distribution release's
    hmailserver_debian12_amd64.deb and hmailserver.fc44.x86_64.rpm, the tarball's
    hmailserver.tar.gz, the chart's hmailserver-chart.tgz, the provenance's
    hmailserver.intoto.sigstore.json); a bundle, its copy and a tree SBOM keep
    theirs, but for a character GitLab refuses in a link's path (link_safe)."""
    if (not is_primary(name) and name != provenance_name(version)) or name.startswith("hmailserver.") and name.endswith(".json"):
        return link_safe(name)
    for marker, replacement in (("-%s-1." % version, "."), ("_%s-1~" % version, "_"), ("-%s" % version, ""),
                                ("_%s" % version, "")):
        if marker in name:
            return link_safe(name.replace(marker, replacement, 1))
    return link_safe(name)


def link_safe(name):
    """A name as a link's path may carry it: GitLab refuses a direct_asset_path with a
    character outside letters, digits, _ . - and /, and a distribution release's .deb
    has the ~ of its version (hmailserver_6.3.6-1~debian12_amd64.deb) in its name and
    in its bundle's. A - stands for it; no name of a release before 6.3.6 has one."""
    return name.replace("~", "-")


def link_type(name):
    return "package" if is_primary(name) and name.endswith((".exe", ".deb", ".rpm", ".AppImage")) else "other"


def registry_url(version, name):
    return "%s/packages/generic/%s/%s/%s" % (feed.API, PACKAGE, urllib.parse.quote(version, safe=""), urllib.parse.quote(name, safe=""))


def release_links(version, names):
    links = []
    for name in sorted(names):
        links.append({"name": name, "url": registry_url(version, name), "link_type": link_type(name),
                      "direct_asset_path": "/downloads/" + permalink_name(name, version)})
    return links


# What GitLab holds a release's links to (app/models/releases/link.rb, read on 28
# September 2026): a direct_asset_path matching FILEPATH_REGEX, of at most 128
# characters, and each link's name, URL and path its own within the release. A link
# that breaks one fails the whole release's creation, after every file is uploaded.
# GitLab sets no limit on the number of links of a release (neither the model, the
# create service nor the API has one), and a release's description may be a million
# characters (Gitlab::Database::MAX_TEXT_SIZE_LIMIT).
GITLAB_LINK_PATH = re.compile(r"\A/[^/](?!.*//.*)[\-.\w/]+[\da-zA-Z]+\Z", re.ASCII)
GITLAB_LINK_PATH_MAX = 128
GITLAB_DESCRIPTION_MAX = 1000000
# And a generic package's file name (lib/gitlab/regex/packages.rb): letters, digits and
# . _ - + ~ @, neither of the last two first or last.
GITLAB_FILE_NAME = re.compile(r"\A(?!~)(?!@)[A-Za-z0-9._\-+~@]+(?<!~)(?<!@)\Z")


def link_problems(links, description=""):
    """What GitLab would refuse of a release with these links and notes, as sentences."""
    problems = []
    for key in ("name", "url", "direct_asset_path"):
        values = [link[key] for link in links]
        twice = sorted(set(v for v in values if values.count(v) > 1))
        if twice:
            problems.append("two links have the %s %s: GitLab takes each once in a release" % (key.replace("_", " "), ", ".join(twice)))
    for link in links:
        path = link["direct_asset_path"]
        if len(path) > GITLAB_LINK_PATH_MAX or not GITLAB_LINK_PATH.match(path):
            problems.append("the link %s would have the path %s, which GitLab refuses (at most %d characters of letters, "
                            "digits, _ . - and /, ending in a letter or a digit)" % (link["name"], path, GITLAB_LINK_PATH_MAX))
        if not GITLAB_FILE_NAME.match(link["name"]):
            problems.append("%s is not a name the generic package registry takes for a file" % link["name"])
    if len(description) > GITLAB_DESCRIPTION_MAX:
        problems.append("the release's notes are %d characters; GitLab takes %d" % (len(description), GITLAB_DESCRIPTION_MAX))
    return problems


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_version_h(root):
    with open(os.path.join(root, VERSION_H), encoding="utf-8") as f:
        text = f.read()
    match = re.search(r'^#define HMAILSERVER_VERSION "([^"]+)"', text, re.MULTILINE)
    if not match:
        raise ReleaseError("%s defines no HMAILSERVER_VERSION" % VERSION_H)
    return match.group(1)


def changelog_section(text, version):
    """The body of CHANGELOG.md's section for version, without its heading."""
    lines = text.replace("\r\n", "\n").split("\n")
    heading = re.compile(r"^## \[" + re.escape(version) + r"\]\(")
    for index, line in enumerate(lines):
        if heading.match(line):
            body = []
            for following in lines[index + 1:]:
                if following.startswith("## ") or following.strip() == "---":
                    break
                body.append(following)
            text = "\n".join(body).strip()
            if text:
                return text
            break
    raise ReleaseError("CHANGELOG.md has no section '## [%s](...)' with anything in it: the release's notes are that "
                       "section (RELEASE.md, the release on GitLab), so write it and tag again" % version)


# ---------------------------------------------------------------- collect

def signer_keys(text):
    """The keys an allowed_signers file lists: (principals, key type, key), one per
    line that is not blank or a comment. Options such as namespaces="git" may stand
    between the principals and the key."""
    keys = []
    for line in text.replace("\r\n", "\n").split("\n"):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        for index, field in enumerate(fields[1:], 1):
            if re.match(r"^(ssh-|ecdsa-|sk-)", field) and index + 1 < len(fields):
                keys.append((fields[0], field, fields[index + 1]))
                break
        else:
            keys.append((fields[0], "", ""))
    return keys


def fingerprint(key):
    """The SHA256 fingerprint ssh-keygen -l prints for a public key given as base64;
    nothing for a key that is missing or not base64."""
    if not key:
        return ""
    try:
        blob = base64.b64decode(key, validate=True)
    except (ValueError, TypeError):
        return ""
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")


def check_tag(root, tag, commit, run=subprocess.run):
    """The tag is an annotated tag object; it points at the commit this pipeline built,
    so a tag deleted and made again on another commit after the pipeline started is
    refused, not signed for; that commit is on master; and the tag is signed by the
    one key master's .gitlab/allowed_signers lists.

    The allow-list is master's, fetched by name, and never the tagged tree's: a tag
    on a commit of its own could carry a key of its own in that file. It must list
    one key, since release tags are the owner's alone (GOVERNANCE.md), and the
    signature must be that key's by its fingerprint as well as by git's word. This
    runs as the tagged commit has it, so what it adds is defence in depth - a
    commit that is not master's can carry another release-chain.py too. Against
    such a commit the protected tag rule holds the Sigstore signature, and the
    Azure credential issued for one tag holds the Authenticode signature alone
    (RELEASE.md)."""
    problems = []

    def git(*arguments):
        return run(["git", "-C", root] + list(arguments), capture_output=True, text=True)

    def said(answer):
        return (answer.stderr or answer.stdout or "exit %d" % answer.returncode).strip()[:400]

    git("fetch", "--force", "--no-tags", "origin", "+refs/tags/%s:refs/tags/%s" % (tag, tag))
    kind = git("cat-file", "-t", "refs/tags/" + tag)
    if kind.returncode != 0 or kind.stdout.strip() != "tag":
        problems.append("%s is not an annotated tag (git cat-file -t says %r): a release's tag is made with "
                        "git tag -s %s -m '...' (RELEASE.md)" % (tag, kind.stdout.strip() or kind.stderr.strip(), tag))
        return problems
    pointed = git("rev-parse", "--verify", "--quiet", "refs/tags/%s^{commit}" % tag)
    target = pointed.stdout.strip() if pointed.returncode == 0 else ""
    if not re.match(r"^[0-9a-f]{40}([0-9a-f]{24})?$", commit or ""):
        problems.append("CI_COMMIT_SHA is %r, not a commit: collect runs in the release job of a v* tag's pipeline and "
                        "nowhere else" % (commit or ""))
    elif target != commit:
        problems.append("%s points at %s now, and this pipeline built %s: the tag was moved after the pipeline started. "
                        "Nothing has been signed; a release is made by the pipeline of the tag as it stands"
                        % (tag, target or "nothing", commit))

    # master, fetched by name. In the job's shallow checkout git fetches master's
    # history down to the tagged commit when it is an ancestor. Another shallow
    # boundary in the checkout can still hide the way down, so a "no" from a shallow
    # repository is asked again with master's whole history.
    fetched = git("fetch", "--no-tags", "origin", "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF))
    if fetched.returncode != 0:
        problems.append("%s could not be fetched, and the allow-list and the tag's ancestry are read from it: %s"
                        % (TRUSTED_BRANCH, said(fetched)))
        return problems
    if target:
        ancestry = git("merge-base", "--is-ancestor", target, TRUSTED_REF)
        if ancestry.returncode == 1 and git("rev-parse", "--is-shallow-repository").stdout.strip() == "true":
            deepened = git("fetch", "--unshallow", "--no-tags", "origin", "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF))
            if deepened.returncode != 0:
                problems.append("%s's whole history could not be fetched to ask whether %s is on it: %s"
                                % (TRUSTED_BRANCH, target, said(deepened)))
                return problems
            ancestry = git("merge-base", "--is-ancestor", target, TRUSTED_REF)
        if ancestry.returncode == 1:
            problems.append("%s points at %s, which is not on %s: a release is tagged on a commit %s already has "
                            "(RELEASE.md, 'Tag the release on master')" % (tag, target, TRUSTED_BRANCH, TRUSTED_BRANCH))
        elif ancestry.returncode != 0:
            problems.append("git could not say whether %s is on %s: %s" % (target, TRUSTED_BRANCH, said(ancestry)))

    listed = git("show", "%s:%s" % (TRUSTED_REF, ALLOWED_SIGNERS))
    if listed.returncode != 0:
        problems.append("%s has no %s to verify the tag against: %s" % (TRUSTED_BRANCH, ALLOWED_SIGNERS, said(listed)))
        return problems
    keys = signer_keys(listed.stdout)
    if len(keys) != 1 or not fingerprint(keys[0][2]):
        problems.append("%s's %s lists %d key(s)%s: release tags are signed by the owner's key alone (GOVERNANCE.md), so "
                        "the job takes a list of exactly one" % (TRUSTED_BRANCH, ALLOWED_SIGNERS, len(keys),
                                                                 "" if len(keys) != 1 else ", and its key is not base64"))
        return problems
    principal, _, key = keys[0]
    wanted = fingerprint(key)

    work = tempfile.mkdtemp(prefix="hm-signers-")
    try:
        signers = os.path.join(work, "allowed_signers")
        with open(signers, "w", encoding="utf-8", newline="\n") as f:
            f.write(listed.stdout)
        verified = git("-c", "gpg.ssh.allowedSignersFile=" + signers.replace(os.sep, "/"), "verify-tag", tag)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if verified.returncode != 0:
        problems.append("%s is not signed by the key %s's %s lists (%s, %s): %s" % (
            tag, TRUSTED_BRANCH, ALLOWED_SIGNERS, principal, wanted, said(verified)))
        return problems
    good = GOOD_SIGNATURE.search((verified.stderr or "") + "\n" + (verified.stdout or ""))
    if not good or good.group(3) != wanted or good.group(1) != principal:
        problems.append("%s is signed by %s, not by the one key %s's %s lists (%s, %s)" % (
            tag, "%s with %s" % (good.group(1), good.group(3)) if good else "a key git does not name", TRUSTED_BRANCH,
            ALLOWED_SIGNERS, principal, wanted))
    return problems


def check_sums(sums_path, deb_path, rpm_path):
    """linux-build's SHA256SUMS.txt names exactly its two packages, the x86-64 .deb and .rpm, with their digests."""
    wanted = {os.path.basename(deb_path): sha256_of(deb_path), os.path.basename(rpm_path): sha256_of(rpm_path)}
    listed = {}
    with open(sums_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            match = re.match(r"^([0-9a-f]{64})\s+\*?(\S+)$", line)
            if not match:
                return ["%s has a line that is not '<sha256>  <file>': %r" % (sums_path, line)]
            listed[match.group(2)] = match.group(1)
    if listed != wanted:
        return ["%s is not the sums of the two x86-64 packages linux-build made: it lists %s" % (sums_path, sorted(listed))]
    return []


def check_job_sums(sums_path, listed_name, package_path):
    """A package job's SHA256SUMS.txt gives the package, under the name the job gave
    it there, the SHA-256 of the file collect found: the bytes the job built."""
    digest = sha256_of(package_path)
    found = None
    with open(sums_path, encoding="utf-8") as f:
        for line in f:
            match = re.match(r"^([0-9a-f]{64})\s+\*?(\S+)\s*$", line.strip())
            if match and match.group(2) == listed_name:
                found = match.group(1)
    if found is None:
        return ["%s does not name %s: it is not the sums of the build that made %s" % (sums_path, listed_name, os.path.basename(package_path))]
    if found != digest:
        return ["%s gives %s the SHA-256 %s, and %s is %s: the file is not the one its build job made" % (
            sums_path, listed_name, found, os.path.basename(package_path), digest)]
    return []


def release_sums(root, version, rehearsal=None):
    """The release's hmailserver-linux-<v>-SHA256SUMS.txt, as bytes: linux-build's
    SHA256SUMS.txt as it is - the x86-64 .deb and .rpm - then a line for every other
    file sums_members names, the AArch64 .deb and .rpm first, in sha256sum's form,
    under the names the release gives them. collect has held every one to its build
    first."""
    places = sources(version, rehearsal)
    with open(os.path.join(root, places[sums_name(version)]), "rb") as f:
        text = f.read()
    if text and not text.endswith(b"\n"):
        text += b"\n"
    for name in sums_members(version):
        text += ("%s  %s\n" % (sha256_of(os.path.join(root, places[name])), name)).encode("ascii")
    return text


def tarball_problems(root, path, commit, run=subprocess.run):
    """build/check-source-tarball.py's verdict on the tarball: its format, the list
    it is held to, its licences, and that git archive recorded this commit in it
    (docs/SourceTarball.md)."""
    answer = run([sys.executable, os.path.join(root, "build", "check-source-tarball.py"), "--commit", commit, path],
                 capture_output=True, text=True)
    if answer.returncode != 0:
        said = "; ".join(l.strip() for l in ((answer.stdout or "") + "\n" + (answer.stderr or "")).split("\n")
                         if l.strip())[-600:]
        return ["%s is not the source tarball of %s that build/check-source-tarball.py takes: %s" % (
            os.path.basename(path), commit, said or "exit %d" % answer.returncode)]
    return []


def rehearsal_problems(env):
    """A rehearsal is a protected batch* branch's pipeline and nothing else: never a
    tag's, whose release job it is not, and never master's."""
    problems = []
    branch = env.get("CI_COMMIT_BRANCH") or ""
    if env.get("CI_COMMIT_TAG"):
        problems.append("this is the pipeline of the tag %s: a tag's release is collected by the release job, never "
                        "rehearsed" % env.get("CI_COMMIT_TAG"))
    if not re.match(r"^batch[A-Za-z0-9._\-]*$", branch) or branch == (env.get("CI_DEFAULT_BRANCH") or "master"):
        problems.append("a release is rehearsed on a protected batch* branch alone, not on %r" % branch)
    if env.get("CI_COMMIT_REF_PROTECTED") != "true":
        problems.append("%s is not protected: only a protected branch's pipeline has the project's runners' files" % branch)
    if not re.match(r"^[0-9a-f]{40}$", env.get("CI_COMMIT_SHA") or ""):
        problems.append("CI_COMMIT_SHA is %r, not a commit" % (env.get("CI_COMMIT_SHA") or ""))
    return problems


def master_exceptions(root, run=subprocess.run):
    """(master's .gitlab/security-exceptions.yml, None), or (None, the problem). check_tag
    fetched master by name; the tagged tree's copy is never read, as its allow-list is not."""
    shown = run(["git", "-C", root, "show", "%s:%s" % (TRUSTED_REF, security_gate.EXCEPTIONS)], capture_output=True, text=True)
    if shown.returncode != 0:
        return None, ("%s's %s could not be read (master not fetched, or without the file), and it says what findings "
                      "of the security scans a release may go out with: the gate reads master's (SECURITY.md, 'Before a "
                      "release'): %s" % (
                          TRUSTED_BRANCH, security_gate.EXCEPTIONS, (shown.stderr or shown.stdout or "").strip()[:300]))
    return shown.stdout, None


def collect(root, tag, out, env, run=subprocess.run, api=None, rehearse=False):
    """Checks everything the rest of the chain relies on, then gathers the files.
    Returns the problems; the files are copied only when there are none.

    A rehearsal (rehearse=True; tag None) runs on a protected batch* branch: the
    version is Version.h's, the installer windows-packages-x64's, and the tag, the
    Authenticode settings, the security scans and the registry are not asked about,
    since nothing will be published. Everything else is held as for a release."""
    problems = []
    if rehearse:
        problems = rehearsal_problems(env)
        if problems:
            return problems
        version = read_version_h(root)
        rehearsal = env.get("CI_COMMIT_SHA")
    else:
        try:
            version = version_of(tag)
        except ReleaseError as e:
            return [str(e)]
        rehearsal = None

        stamped = read_version_h(root)
        if stamped != version:
            problems.append("Version.h says %s and the tag says %s: every file name is made from Version.h, so the release "
                            "would carry names no server looks for (RELEASE.md, the version stamp)" % (stamped, version))

    image = env.get("HM_COSIGN_IMAGE", "")
    if not PINNED_IMAGE.match(image):
        problems.append("HM_COSIGN_IMAGE is %r, not gcr.io/projectsigstore/cosign pinned by digest: set the digest of "
                        "its -dev tag in .gitlab-ci.yml (docs/ContinuousIntegration.md, \"The release\"), and remove any "
                        "CI/CD variable of that name, which replaces the file's value" % image)

    if not rehearse:
        missing = [name for name in AZURE_SETTINGS if not (env.get(name) or "").strip()]
        if missing:
            problems.append("the Authenticode job needs the CI/CD variables %s, which are not set for this tag's pipeline "
                            "(RELEASE.md, \"Authenticode\"). Nothing has been signed." % ", ".join(missing))

        problems.extend(check_tag(root, tag, env.get("CI_COMMIT_SHA", ""), run))

        # The security scans of this commit, read before anything is signed: static-analysis,
        # semgrep, semgrep-sast and dependency-scan, whose artifacts the job needs. A scan that
        # did not finish, or a finding the policy counts that is in no baseline and no
        # exception in force, holds the release (SECURITY.md, "Before a release"). The
        # exceptions are master's, as check_tag fetched it, like the allow-list: a finding the
        # owner accepts on master after the tag counts when the job is retried.
        exceptions, missing = master_exceptions(root, run)
        if missing:
            problems.append(missing)
        problems.extend(security_gate.evaluate(root, root, exceptions_text=exceptions or "exceptions: []\n", say=say))

    places = sources(version, rehearsal)
    job_sums = package_job_sums(version, rehearsal)
    for name in collected_assets(version):
        path = os.path.join(root, places[name])
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            problems.append("%s is not in this job's artifacts (%s): the job that makes it failed or did not run" % (name, places[name]))
    for name, (relative, _) in sorted(job_sums.items()):
        if not os.path.isfile(os.path.join(root, relative)):
            problems.append("%s, the sums of the build that made %s, is not in this job's artifacts: the job that makes it "
                            "failed or did not run" % (relative, name))
    if not problems:
        names = feed.installer_names(version)
        problems.extend(check_sums(os.path.join(root, places[sums_name(version)]),
                                   os.path.join(root, places[names["deb-amd64"]]),
                                   os.path.join(root, places[names["rpm-x86_64"]])))
        problems.extend(sbom_problems(os.path.join(root, "linux-build", "packages"), version,
                                      [names["deb-amd64"], names["rpm-x86_64"]]))
        # Every other package, AppImage and the chart: each the bytes its job built, by
        # the sums that job wrote, and each compiled one with its SBOM beside it.
        for name, (relative, listed) in sorted(job_sums.items()):
            problems.extend(check_job_sums(os.path.join(root, relative), listed, os.path.join(root, places[name])))
            if name in compiled_assets(version) and name != names["windows"]:
                problems.extend(sbom_problems(os.path.dirname(os.path.join(root, places[name])), version, [name]))
        # The tarball has no sums of its own job: it is held to this commit instead.
        problems.extend(tarball_problems(root, os.path.join(root, places[tarball_name(version)]), env.get("CI_COMMIT_SHA", ""), run))
        problems.extend(sbom_problems(os.path.join(root, GATHERED), version, [tarball_name(version)]))
    sums = None
    if not problems:
        sums = release_sums(root, version, rehearsal)
        digests = dict((name, sha256_of(os.path.join(root, places[name]))) for name in collected_assets(version))
        digests[sums_name(version)] = hashlib.sha256(sums).hexdigest()
        sizes = dict((name, os.path.getsize(os.path.join(root, places[name]))) for name in collected_assets(version))
        sizes[sums_name(version)] = len(sums)
        if not rehearse:
            problems.extend(check_registry_before_signing(api, version, digests))
        # What GitLab and Pages take, before anything is signed: the release's links,
        # and the site the feed and the apt and dnf repositories make with it (in a
        # rehearsal, with this release alone).
        if not problems:
            problems.extend(check_budget(api if not rehearse else None, "v" + version if rehearse else tag, version,
                                         digests, sizes, keep=repository_keep(root)))
    if problems:
        return problems

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for name in collected_assets(version):
        if name == sums_name(version):
            with open(os.path.join(out, name), "wb") as f:
                f.write(sums)
        else:
            shutil.copyfile(os.path.join(root, places[name]), os.path.join(out, name))
        say("  %s  %12d  %s" % (sha256_of(os.path.join(out, name)), os.path.getsize(os.path.join(out, name)), name))
    return []


# ---------------------------------------------------------------- GitLab

class GitLab(object):
    """The API as the job's own token: JOB-TOKEN, sent as a header, never printed."""

    def __init__(self, token):
        self.token = token

    def request(self, method, url, body=None, content_type=None):
        headers = {"User-Agent": "hmailserver-release-chain", "Accept": "application/json"}
        if self.token:
            headers["JOB-TOKEN"] = self.token
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(url, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=600) as response:
                return response.status, response.read(), dict((k.lower(), v) for k, v in response.headers.items())
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict((k.lower(), v) for k, v in e.headers.items())
        except (urllib.error.URLError, OSError) as e:
            raise ReleaseError("%s %s could not be reached: %s" % (method, url.split("?")[0], e))

    def feed_get(self, url):
        """build/generate-update-feed.py's get: (body, headers), or FeedError."""
        status, body, headers = self.request("GET", url)
        if status != 200:
            raise feed.FeedError("%s answered HTTP %d" % (url.split("?")[0], status))
        return body, headers


def message_of(body):
    try:
        value = json.loads(body.decode("utf-8"))
        return json.dumps(value.get("message") or value.get("error") or value)[:300] if isinstance(value, dict) else str(value)[:300]
    except (ValueError, UnicodeDecodeError):
        return body[:200].decode("utf-8", "replace")


def registry_files(api, version):
    """Every record the registry holds of each file of hmailserver/<version>, oldest
    first. A generic package can hold a name more than once, and a download by name
    is served the newest, so each is read."""
    packages = feed.get_all(api.feed_get, "/packages?package_type=generic&package_name=%s" % PACKAGE)
    held = {}
    for package in packages:
        if package.get("name") != PACKAGE or package.get("version") != version:
            continue
        for entry in feed.get_all(api.feed_get, "/packages/%d/package_files" % package["id"]):
            held.setdefault(entry.get("file_name"), []).append(entry)
    for entries in held.values():
        entries.sort(key=lambda e: e.get("id", 0))
    return held


def registry_conflicts(held, digests, version):
    """The files of digests ({name: sha256}) the registry already holds with other
    bytes, as problems, and the names it holds with these bytes and no other."""
    problems = []
    same = set()
    for name in sorted(digests):
        entries = held.get(name) or []
        other = [e for e in entries if (e.get("file_sha256") or "").lower() != digests[name]]
        if other:
            problems.append("the package %s/%s already holds %s with the SHA-256 %s, not %s, this release's: a file name "
                            "is a release's once, and the link a server downloads by name is served the newest. Nothing "
                            "has been uploaded (RELEASE.md, 'A tag is spent once')" % (
                                PACKAGE, version, name, other[0].get("file_sha256") or "unknown", digests[name]))
        elif entries:
            same.add(name)
    return problems, same


def check_registry_before_signing(api, version, digests):
    """collect's look at the registry. The .deb, the .rpm, the sums and the SBOMs are
    final here, and the registry may hold them only with these bytes, as it does
    after a release-publish that stopped part-way. The installer and every bundle are
    made by the jobs after this one (Authenticode, Sigstore), and so are the bundles'
    .sigstore.json copies and the provenance; a second signing never makes the same
    bytes, so the registry may not hold them at all: such a release is finished by
    retrying release-publish of the pipeline that signed them."""
    if api is None:
        return ["there is no CI_JOB_TOKEN to read the package registry with: collect runs in the release job and nowhere else"]
    try:
        held = registry_files(api, version)
    except (ReleaseError, feed.FeedError) as e:
        return ["the package registry could not be read, and a release is not signed without knowing what it holds "
                "under %s/%s: %s" % (PACKAGE, version, e)]
    later = made_after_collect(version)
    final = dict((name, digest) for name, digest in digests.items() if name not in later)
    problems, _ = registry_conflicts(held, final, version)
    signed_later = later + sorted(n + BUNDLE for n in set(digests) | set(later)) + sorted(
        n + BUNDLE_COPY for n in primary_assets(version)) + [provenance_name(version)]
    for name in signed_later:
        if held.get(name):
            problems.append("the package %s/%s already holds %s, which an earlier run's signing made: signing again makes "
                            "other bytes, which release-publish refuses. Retry release-publish in the pipeline that signed "
                            "it, or, if that cannot be, the tag is spent (RELEASE.md, 'A tag is spent once')" % (
                                PACKAGE, version, name))
    return problems


# ---------------------------------------------------------------- what GitLab and Pages take

# The largest feed document a server reads: UpdateChecker.cpp's MAX_FEED_BYTES, which
# build/check-release-chain.py holds this to. releases.json lists every release, and a
# release as 6.3.6 is adds some fifty kilobytes to it.
MAX_FEED_BYTES = 4 * 1024 * 1024
# What a file not made yet is counted as: a bundle is about 7 KB, an SBOM up to 60 KB.
ESTIMATED_SIZE = 99999
# The job that adds the apt and dnf repositories' rules to the site, and the one
# command of it that makes them: its --keep, the newest stable releases it keeps
# (none given: every one), is what check_budget counts the repositories with, so that
# the answer the refusal names - a --keep there - is one this count takes too.
REPOS_JOB_FILE = os.path.join(".gitlab", "ci", "package-repos.yml")


def repository_keep(root):
    """The --keep of build/generate-package-repos.py --site in the package-repos job
    (0, every stable release, when it gives none or the file cannot be read, which
    counts the most rules and so refuses rather than passes)."""
    try:
        with open(os.path.join(root, REPOS_JOB_FILE), encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return 0
    for line in text.replace("\r\n", "\n").split("\n"):
        if "generate-package-repos.py" in line and "--site" in line and not line.lstrip().startswith("#"):
            match = re.search(r"--keep(?:=|\s+)([0-9]+)\b", line)
            return int(match.group(1)) if match else 0
    return 0


def check_budget(api, tag, version, digests, sizes, out=None, keep=0):
    """What GitLab and Pages would refuse of this release, before anything is signed
    or uploaded: every link GitLab must take (link_problems), and the site the pages
    job makes with the release in it - the update feed, whose _redirects the apt and
    dnf repositories' rules are added to (build/generate-package-repos.py, keeping
    the newest keep stable releases, or every one for 0, as the package-repos job
    is told: repository_keep), which Pages reads only up to 1,000 rules and 64 KiB,
    and releases.json and releases/latest, which a server reads only up to
    MAX_FEED_BYTES. The site is rendered from the live releases (api), or, where
    there is no api (a rehearsal), from this release alone. A file of the release not
    made yet counts with a placeholder digest and size. Returns the problems; says
    the counts."""
    out = out or say
    names = release_files(version)
    links = release_links(version, names)
    problems = link_problems(links)
    releases, files = [], {}
    if api is not None:
        try:
            releases, files = feed.load(api.feed_get)
        except (ReleaseError, feed.FeedError) as e:
            return problems + ["the live releases could not be read to count the site with this release in it: %s" % e]
    releases = [r for r in releases if r.get("tag_name") != tag]
    files = dict(files)
    record = {}
    for number, name in enumerate(sorted(names)):
        record[name] = {"id": 1000000000 + number, "file_name": name, "size": sizes.get(name, ESTIMATED_SIZE),
                        "file_sha256": digests.get(name, "0" * 64)}
    files[(PACKAGE, version)] = record
    release = {"tag_name": tag, "name": "hMailServer %s" % version, "upcoming_release": False,
               "released_at": "2099-12-31T23:59:59.000Z", "_links": {"self": feed.PROJECT_URL + "/-/releases/" + tag},
               "assets": {"links": links}}
    try:
        tree, latest, warnings = feed.render([release] + releases, files)
    except feed.FeedError as e:
        return problems + ["the update feed would not take %s: %s" % (tag, e)]
    problems.extend(w for w in warnings if w.startswith(tag + " ") or (tag + ":") in w)
    feed_rules, errors = feed.parse_redirects(tree["_redirects"])
    problems.extend(errors)
    entries, _ = repos.select(json.loads(tree["releases.json"].decode("utf-8")), keep=keep)
    text, added, count = repos.redirects_text(tree["_redirects"].decode("utf-8"), entries)
    size = len(text.encode("utf-8"))
    if size > repos.REDIRECTS_MAX_BYTES or count > repos.REDIRECTS_MAX_RULES:
        problems.append("the site's _redirects would hold %d rules in %d bytes, the feed's %d and the apt and dnf "
                        "repositories' %d (%s); Pages reads %d rules and %d bytes, and nothing of a larger file. Give "
                        "build/generate-package-repos.py --keep in .gitlab/ci/package-repos.yml, or a smaller one, which "
                        "this count reads from the tree it runs in (docs/PackageManagers.md, 'What the repositories hold')" % (
                            count, size, len(feed_rules), added,
                            "the newest %d stable releases, as its --keep says" % keep if keep else "every stable release: it has no --keep",
                            repos.REDIRECTS_MAX_RULES, repos.REDIRECTS_MAX_BYTES))
    for document in ("releases.json", "releases/latest"):
        if len(tree[document]) > MAX_FEED_BYTES:
            problems.append("the feed's %s would be %d bytes; a server reads %d (UpdateChecker.cpp, MAX_FEED_BYTES)" % (
                document, len(tree[document]), MAX_FEED_BYTES))
    own = [e for e in entries if e["version"] == version]
    out("the release: %d files and %d links (%d primary files, %d SBOMs, %d bundles, %d .sigstore.json copies, "
        "the provenance)" % (len(expected_assets(version)), len(links), len(primary_assets(version)),
                             len(compiled_assets(version)), len(expected_assets(version)), len(primary_assets(version))))
    out("the site with it: _redirects %d rules in %d bytes (the feed's %d, the repositories' %d, %d of them this "
        "release's; Pages reads %d rules and %d bytes); releases.json %d bytes and releases/latest %d (a server reads "
        "%d)" % (count, size, len(feed_rules), added, len(own), repos.REDIRECTS_MAX_RULES, repos.REDIRECTS_MAX_BYTES,
                 len(tree["releases.json"]), len(tree["releases/latest"]), MAX_FEED_BYTES))
    return problems


# ---------------------------------------------------------------- gather: the release-packages job

def default_sbom_maker(root, version, env=None, run=subprocess.run):
    """Each gathered file's SBOM, beside it: a package's and an AppImage's by
    build/ci/packages/package-sbom.sh from what its build recorded, with one component
    list for all of them; the tarball's by build/asset-sbom.py from the tarball alone.
    Runs in the CI image, which has PowerShell, Python, dpkg-deb and rpm."""
    env = dict(env if env is not None else os.environ)
    # The documents' timestamp is the commit's, as package-sbom.sh sets it for its own.
    if env.get("CI_COMMIT_TIMESTAMP") and not env.get("SOURCE_DATE_EPOCH"):
        try:
            moment = datetime.datetime.fromisoformat(env["CI_COMMIT_TIMESTAMP"].replace("Z", "+00:00"))
            env["SOURCE_DATE_EPOCH"] = str(int(moment.timestamp()))
        except ValueError:
            pass
    state = {}

    def dependencies():
        if "path" not in state:
            state["dir"] = tempfile.mkdtemp(prefix="hm-gather-")
            state["path"] = os.path.join(state["dir"], "native-dependencies.json")
            answer = run(["pwsh", "-NoProfile", "-File", os.path.join(root, "build", "native-dependencies.ps1"),
                          "-OutputFile", state["path"]], cwd=root, capture_output=True, text=True)
            if answer.returncode != 0 or not os.path.isfile(state["path"]):
                raise ReleaseError("build/native-dependencies.ps1 could not make the component list: %s" % (
                    (answer.stdout or "") + (answer.stderr or "")).strip()[-400:])
        return state["path"]

    def make(build, path):
        if not build["sbom"]:
            raise ReleaseError("%s is source, which goes out without an SBOM" % build["name"])
        if build["kind"] == "source":
            command = [sys.executable, os.path.join(root, "build", "asset-sbom.py"), "make", "--kind", "source",
                       "--asset", path, "--version", version]
            answer = run(command, cwd=root, capture_output=True, text=True, env=env)
        else:
            command = ["bash", os.path.join("build", "ci", "packages", "package-sbom.sh"), path,
                       os.path.join(root, local(build["record"]))]
            answer = run(command, cwd=root, capture_output=True, text=True, env=dict(env, HM_NATIVE_DEPENDENCIES=dependencies()))
        if answer.returncode != 0:
            raise ReleaseError("the SBOM of %s could not be made: %s" % (build["name"], (
                (answer.stdout or "") + (answer.stderr or "")).strip()[-600:]))

    return make


def gather(root, version, out, commit, make_sbom=None, run=subprocess.run):
    """The release-packages job: every file gathered_builds names, from the artifacts
    of the jobs that made them, each held to the sums its job wrote - the tarball to
    build/check-source-tarball.py and this commit - copied under its name into out,
    with that job's sums under out/job-sums/<job>.txt and, but for the runtime's
    sources, its SBOM beside it, which must describe it. Returns the problems; out
    holds the files only when there are none."""
    problems = []
    builds = gathered_builds(version)
    for build in builds:
        path = os.path.join(root, local(build["path"]))
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            problems.append("%s is not in this job's artifacts (%s): %s failed or did not run" % (build["name"], build["path"], build["job"]))
        if build["sums"] and not os.path.isfile(os.path.join(root, local(build["sums"]))):
            problems.append("%s, the sums of %s, is not in this job's artifacts" % (build["sums"], build["job"]))
        if build["record"] and not os.path.isdir(os.path.join(root, local(build["record"]))):
            problems.append("%s, what %s was built against, is not in this job's artifacts: its SBOM is made from it"
                            % (build["record"], build["name"]))
    if problems:
        return problems
    for build in builds:
        path = os.path.join(root, local(build["path"]))
        if build["sums"]:
            problems.extend(check_job_sums(os.path.join(root, local(build["sums"])), build["listed"], path))
        else:
            problems.extend(tarball_problems(root, path, commit, run))
    if problems:
        return problems

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, JOB_SUMS))
    for build in builds:
        shutil.copyfile(os.path.join(root, local(build["path"])), os.path.join(out, build["name"]))
        if build["sums"]:
            shutil.copyfile(os.path.join(root, local(build["sums"])), os.path.join(out, JOB_SUMS, build["job"] + ".txt"))
    maker = make_sbom or default_sbom_maker(root, version, run=run)
    for build in builds:
        if not build["sbom"]:
            continue
        try:
            maker(build, os.path.join(out, build["name"]))
        except (ReleaseError, OSError) as e:
            problems.append(str(e))
            continue
        problems.extend(sbom_problems(out, version, [build["name"]]))
    if not problems:
        for build in builds:
            say("  %s  %12d  %s (%s)" % (sha256_of(os.path.join(out, build["name"])), os.path.getsize(os.path.join(out, build["name"])),
                                          build["name"], build["job"]))
    return problems


# ---------------------------------------------------------------- the provenance

# The paths a server trusts a signature from, and the repository each names: the
# project's now, and the group's it is to move to (release-sign.sh's case on
# CI_PROJECT_PATH, which build/check-release-chain.py holds this to).
SIGNING_REPOSITORIES = {"Progressiverobot/hmailserver": "https://gitlab.com/Progressiverobot/hmailserver",
                        "hmailserver/hmailserver": "https://gitlab.com/hmailserver/hmailserver"}
IN_TOTO_STATEMENT = "https://in-toto.io/Statement/v1"
SLSA_PROVENANCE = "https://slsa.dev/provenance/v1"
IN_TOTO_PAYLOAD = "application/vnd.in-toto+json"
# What the build type names: how this project's pipeline builds a release, documented.
BUILD_TYPE = feed.PROJECT_URL + "/-/blob/master/hmailserver/docs/ContinuousIntegration.md#the-release"


def signing_identity(env, rehearse=False):
    """The identity Fulcio writes for this pipeline: <repository>//.gitlab-ci.yml@
    refs/tags/<tag>, or in a rehearsal refs/heads/<branch>, which no server trusts."""
    repository = SIGNING_REPOSITORIES.get(env.get("CI_PROJECT_PATH") or "")
    if not repository:
        raise ReleaseError("the project's path is %r, not one a server trusts a signature from" % env.get("CI_PROJECT_PATH"))
    if rehearse:
        problems = rehearsal_problems(env)
        if problems:
            raise ReleaseError("; ".join(problems))
        return "%s//.gitlab-ci.yml@refs/heads/%s" % (repository, env["CI_COMMIT_BRANCH"])
    tag = env.get("CI_COMMIT_TAG") or ""
    version_of(tag)
    return "%s//.gitlab-ci.yml@refs/tags/%s" % (repository, tag)


def provenance_statement(version, identity, digests, env):
    """The in-toto statement with the SLSA v1 predicate over every file of
    expected_assets (digests: {name: sha256}): what built them - this project's
    pipeline for the ref, by its .gitlab-ci.yml - from which commit, and which
    pipeline. Its signature (release-sign.sh, cosign attest-blob) binds it to the
    pipeline's identity; a download is held to it with cosign verify-blob-attestation
    (SECURITY.md, "Build provenance")."""
    repository, ref = identity.split("//.gitlab-ci.yml@", 1)
    commit = env.get("CI_COMMIT_SHA") or ""
    metadata = {"invocationId": env.get("CI_PIPELINE_URL") or ""}
    if feed.TIMESTAMP.match(env.get("CI_PIPELINE_CREATED_AT") or ""):
        metadata["startedOn"] = env["CI_PIPELINE_CREATED_AT"]
    return {
        "_type": IN_TOTO_STATEMENT,
        "subject": [{"name": name, "digest": {"sha256": digests[name]}} for name in sorted(digests)],
        "predicateType": SLSA_PROVENANCE,
        "predicate": {
            "buildDefinition": {
                "buildType": BUILD_TYPE,
                "externalParameters": {"repository": repository, "ref": ref, "config": ".gitlab-ci.yml"},
                "internalParameters": {"projectId": env.get("CI_PROJECT_ID") or "", "pipelineId": env.get("CI_PIPELINE_ID") or "",
                                       "pipelineSource": env.get("CI_PIPELINE_SOURCE") or "",
                                       "rehearsal": ref.startswith("refs/heads/")},
                "resolvedDependencies": [{"uri": "git+%s@%s" % (repository, ref), "digest": {"gitCommit": commit}}],
            },
            "runDetails": {"builder": {"id": identity}, "metadata": metadata},
        },
    }


def write_statement(version, identity, files, env, out):
    """release-sbom's second file: the statement over files ({name: path}), which must
    be every file of expected_assets, written to out/statement_name for release-sign."""
    if sorted(files) != sorted(expected_assets(version)):
        raise ReleaseError("the provenance would name %s, and a release is %s" % (sorted(files), sorted(expected_assets(version))))
    digests = dict((name, sha256_of(path)) for name, path in files.items())
    statement = provenance_statement(version, identity, digests, env)
    path = os.path.join(out, statement_name(version))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(statement, f, indent=1, sort_keys=True)
        f.write("\n")
    say("  %s: the SLSA v1 provenance of %d files, built by %s" % (statement_name(version), len(digests), identity))
    return path


def provenance_problems(assets, version, identity, commit):
    """The provenance in assets is a Sigstore bundle holding a DSSE envelope of an
    in-toto statement with the SLSA v1 predicate; the statement names every file of
    expected_assets in assets by its SHA-256, and nothing else; it names this
    identity as the builder and this commit as the source; its certificate names the
    identity, and its log entry is a dsse entry of the public log. cosign verified
    the signature in release-sign.sh; this holds what was signed to the release."""
    path = os.path.join(assets, provenance_name(version))
    try:
        with open(path, encoding="utf-8") as f:
            bundle = json.load(f)
        envelope = bundle["dsseEnvelope"]
        statement = json.loads(base64.b64decode(envelope["payload"]).decode("utf-8"))
        payload_type = envelope.get("payloadType")
    except (OSError, ValueError, KeyError, TypeError) as e:
        return ["%s is not a Sigstore bundle of an in-toto statement: %s" % (provenance_name(version), e)]
    problems = []
    if not str(bundle.get("mediaType", "")).startswith("application/vnd.dev.sigstore.bundle"):
        problems.append("%s is not a Sigstore bundle" % provenance_name(version))
    if payload_type != IN_TOTO_PAYLOAD or statement.get("_type") != IN_TOTO_STATEMENT or statement.get("predicateType") != SLSA_PROVENANCE:
        problems.append("%s is not an in-toto statement with the SLSA v1 predicate (%s, %s, %s)" % (
            provenance_name(version), payload_type, statement.get("_type"), statement.get("predicateType")))
    named = {}
    for subject in statement.get("subject") or []:
        named[subject.get("name")] = (subject.get("digest") or {}).get("sha256")
    wanted = dict((name, sha256_of(os.path.join(assets, name))) for name in expected_assets(version)
                  if os.path.isfile(os.path.join(assets, name)))
    if named != wanted:
        wrong = sorted(n for n in set(named) | set(wanted) if named.get(n) != wanted.get(n))
        problems.append("the provenance does not name every file of the release by its SHA-256, and nothing else: %s" % ", ".join(wrong))
    predicate = statement.get("predicate") or {}
    builder = ((predicate.get("runDetails") or {}).get("builder") or {}).get("id")
    if builder != identity:
        problems.append("the provenance names the builder %s, not %s" % (builder, identity))
    sources = (predicate.get("buildDefinition") or {}).get("resolvedDependencies") or []
    if not any((d.get("digest") or {}).get("gitCommit") == commit for d in sources):
        problems.append("the provenance does not name %s as the source it was built from" % commit)
    # The identity is the certificate's subject alternative name, read from its
    # extensions: a search of its bytes would find it in a public key that holds it.
    try:
        named = sigstore_certificate.uris(sigstore_certificate.bundle_certificate(bundle))
    except ValueError as error:
        named = ["(%s)" % error]
    if identity not in named:
        problems.append("the provenance's certificate does not name %s: its subject alternative name is %s"
                        % (identity, ", ".join(named) or "none"))
    entries = (bundle.get("verificationMaterial") or {}).get("tlogEntries") or []
    if not entries or (entries[0].get("kindVersion") or {}).get("kind") != "dsse" or not entries[0].get("integratedTime"):
        problems.append("the provenance's log entry is not a dsse entry of the public log with its integratedTime")
    return problems


def copy_problems(assets, version):
    """Each primary file's <file>.sigstore.json is its <file>.cosign.bundle, byte for byte."""
    problems = []
    for name in primary_assets(version):
        bundle, copy = os.path.join(assets, name + BUNDLE), os.path.join(assets, name + BUNDLE_COPY)
        if not os.path.isfile(copy) or not os.path.isfile(bundle) or sha256_of(copy) != sha256_of(bundle):
            problems.append("%s is not %s byte for byte" % (name + BUNDLE_COPY, name + BUNDLE))
    return problems


def make_installer_sbom(root, tag, env, assets, installer_path, dependencies_path, out, rehearse=False):
    """release-sbom: the installer's own SBOM, from the bytes Authenticode signed. A
    rehearsal has no Authenticode: its installer is collect's, as it is (tag is None)."""
    version = read_version_h(root) if rehearse else version_of(tag)
    installer = feed.installer_names(version)["windows"]
    if os.path.basename(installer_path) != installer or not os.path.isfile(installer_path):
        raise ReleaseError("%s is not the signed installer %s: release-authenticode's artifact did not arrive" % (installer_path, installer))
    unsigned = os.path.join(assets, installer)
    if not rehearse and os.path.isfile(unsigned) and sha256_of(unsigned) == sha256_of(installer_path):
        raise ReleaseError("%s is byte for byte the installer collect gathered: Authenticode signed nothing, and the SBOM "
                           "would describe bytes that do not ship" % installer_path)
    try:
        with open(dependencies_path, encoding="utf-8-sig") as f:
            dependencies = json.load(f)
        document = asset_sbom.make("windows-x64", installer_path, dependencies, root=root, version=version,
                                   commit=env.get("CI_COMMIT_SHA", ""))
    except (OSError, ValueError, asset_sbom.SbomError) as e:
        raise ReleaseError("the installer's SBOM could not be made: %s" % e)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    path = os.path.join(out, sbom_name(installer))
    asset_sbom.write_document(document, path)
    say("  %s  %d components, describing %s (SHA-256 %s)" % (sbom_name(installer), len(document["components"]), installer,
                                                            sha256_of(installer_path)))
    return path


def create_release(api, payload, version):
    """POSTs the release in the milestone of its version, so the release page lists
    the issues closed for it, and again without one when GitLab refuses the
    milestone, as it does when the project has none of that title. The title is
    named in the request, not looked up first: a job's token may use the Releases
    API, which finds the milestone as the job's user, and not the Milestones API
    (GitLab's job token documentation lists the APIs it may use). Never a reason to
    stop: the release matters, the link does not. Leaves payload as it was last
    sent, and returns GitLab's status and body."""
    payload["milestones"] = [version]
    status, body, _ = api.request("POST", feed.API + "/releases", json.dumps(payload).encode("utf-8"), "application/json")
    if status in (400, 422) and "milestone" in message_of(body).lower():
        say("no milestone linked: creating the release in the milestone %s answered HTTP %d: %s. Creating it without one"
            % (version, status, message_of(body)))
        del payload["milestones"]
        status, body, _ = api.request("POST", feed.API + "/releases", json.dumps(payload).encode("utf-8"), "application/json")
    return status, body


def publish(assets, tag, env, api, notes, verification, render_check=True):
    """Uploads, reads back, creates the release, and checks the feed takes it."""
    version = version_of(tag)
    if env.get("CI_PROJECT_ID") != PROJECT_ID:
        raise ReleaseError("this is project %s, not %s: a release is published from the project the servers trust "
                           "and nowhere else" % (env.get("CI_PROJECT_ID"), PROJECT_ID))
    if (env.get("CI_API_V4_URL") or "").rstrip("/") != API_BASE:
        raise ReleaseError("the API is %s, not %s, which every release link names" % (env.get("CI_API_V4_URL"), API_BASE))

    expected = expected_assets(version)
    wanted = set(release_files(version))
    present = set(n for n in os.listdir(assets) if os.path.isfile(os.path.join(assets, n)))
    if present != wanted:
        parts = []
        if wanted - present:
            parts.append("missing " + ", ".join(sorted(wanted - present)))
        if present - wanted:
            parts.append("extra " + ", ".join(sorted(present - wanted)))
        raise ReleaseError("%s is not a release (%s): a release is exactly %s, a bundle beside each, a .sigstore.json "
                           "copy of each primary file's bundle, and the provenance %s" % (
                               assets, "; ".join(parts), ", ".join(expected), provenance_name(version)))

    unfit = sbom_problems(assets, version) + copy_problems(assets, version) + provenance_problems(
        assets, version, signing_identity(dict(env, CI_COMMIT_TAG=tag)), env.get("CI_COMMIT_SHA") or "")
    if unfit:
        raise ReleaseError("; ".join(unfit) + ". Nothing was uploaded")
    # What GitLab would refuse of the release is refused here, before any upload: a
    # release whose creation fails after its files are up leaves the tag half-spent.
    description = "# hMailServer %s\n\n%s\n\n%s\n" % (version, notes.strip(), verification.strip())
    unfit = link_problems(release_links(version, wanted), description)
    if unfit:
        raise ReleaseError("; ".join(unfit) + ". Nothing was uploaded")

    status, body, _ = api.request("GET", "%s/releases/%s" % (feed.API, urllib.parse.quote(tag, safe="")))
    if status == 200:
        raise ReleaseError("the release %s exists already. A tag is spent once: nothing was uploaded, and nothing on "
                           "it was changed" % tag)
    if status != 404:
        raise ReleaseError("asking whether %s exists answered HTTP %d: %s" % (tag, status, message_of(body)))

    digests = dict((name, sha256_of(os.path.join(assets, name))) for name in wanted)
    unfit = check_budget(api, tag, version, digests, dict((name, os.path.getsize(os.path.join(assets, name))) for name in wanted),
                         keep=repository_keep(ROOT))
    if unfit:
        raise ReleaseError("; ".join(unfit) + ". Nothing was uploaded")
    # A name the package holds with other bytes is refused before anything is sent;
    # one it holds with these bytes, from a run of this job that stopped part-way,
    # is not sent again.
    conflicts, same = registry_conflicts(registry_files(api, version), digests, version)
    if conflicts:
        raise ReleaseError("; ".join(conflicts))
    for name in sorted(wanted):
        if name in same:
            say("  %s is in the registry with these bytes already, from an earlier run: not uploaded again" % name)
            continue
        with open(os.path.join(assets, name), "rb") as f:
            data = f.read()
        status, body, _ = api.request("PUT", registry_url(version, name) + "?select=package_file", data, "application/octet-stream")
        if status not in (200, 201):
            raise ReleaseError("uploading %s answered HTTP %d: %s. Nothing is released until every file is up; retry "
                               "this job, which does not send again what is up already" % (name, status, message_of(body)))
        say("  uploaded %s (%d bytes)" % (name, len(data)))

    # Every record of each name, since a download by name is served the newest.
    held = registry_files(api, version)
    for name in sorted(wanted):
        entries = held.get(name) or []
        if not entries:
            raise ReleaseError("the registry has no %s in %s/%s after its upload" % (name, PACKAGE, version))
        for entry in entries:
            if (entry.get("file_sha256") or "").lower() != digests[name]:
                raise ReleaseError("the registry's %s has the SHA-256 %s, not %s, the file that was signed" % (
                    name, entry.get("file_sha256"), digests[name]))
    say("the registry holds all %d files with the digests that were signed, and no other" % len(wanted))

    payload = {"tag_name": tag, "name": "hMailServer %s" % version, "description": description,
               "assets": {"links": release_links(version, wanted)}}
    status, body = create_release(api, payload, version)
    if status not in (200, 201):
        raise ReleaseError("creating the release %s answered HTTP %d: %s. The files are in the registry; run the job "
                           "again once the cause is fixed" % (tag, status, message_of(body)))
    say("created the release %s with %d links%s" % (tag, len(wanted), ", in the milestone " + version if "milestones" in payload else ""))

    if render_check:
        try:
            check_feed_takes(api, tag, version)
        except (ReleaseError, feed.FeedError) as e:
            raise ReleaseError("the release %s is created, but %s. Servers are not offered it until that is put right: "
                               "read the release's links against docs/UpdateFeed.md, 'What the site serves'" % (tag, e))
    return payload


# The kinds of server a release carries a file for (generate-update-feed.py's
# installer_names): the feed must give each of them its file and its bundle.
SERVED_PLATFORMS = ("windows", "deb-amd64", "rpm-x86_64", "deb-arm64", "rpm-aarch64", "appimage-x86_64", "appimage-aarch64")


def check_feed_takes(api, tag, version):
    """The feed the pages job will render includes this release, and a Windows
    server, a Debian and an RPM server on x86-64 and on AArch64, and an AppImage on
    either, each find its file and bundle in it."""
    releases, files = feed.load(api.feed_get)
    tree, latest, warnings = feed.render(releases, files)
    ours = [w for w in warnings if w.startswith(tag + " ") or (tag + ":") in w]
    key = "releases/tags/" + tag
    if key not in tree or ours:
        raise ReleaseError("the feed leaves %s out: %s" % (tag, "; ".join(ours) or "it is not in the render"))
    document = json.loads(tree[key].decode("utf-8"))
    for platform in SERVED_PLATFORMS:
        release = feed.read_release(document, platform)
        if not release or not release["installer_url"] or not release["bundle_url"] or not release["installer_digest"]:
            raise ReleaseError("a %s server would find no file with its bundle in %s" % (platform, tag))
    say("the feed takes %s: Windows servers, Debian and RPM servers on x86-64 and AArch64, and AppImages on either, "
        "each find their file and its bundle; releases/latest is %s" % (tag, latest["tag_name"]))


# ---------------------------------------------------------------- the self-test

def self_test():
    QUIET.append(True)
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    version = "6.3.5"
    tag = "v6.3.5"
    names = feed.installer_names(version)

    # The names and links, held to the feed's own rules.
    expect(expected_assets(version)[0] == "hMailServer-6.3.5-x64.exe", "the installer is named as a Windows server looks for it")
    for name in expected_assets(version):
        for file_name in (name, name + ".cosign.bundle"):
            match = feed.REGISTRY_LINK.match(registry_url(version, file_name))
            expect(match is not None and urllib.parse.unquote(match.group(3)) == file_name
                   and (urllib.parse.unquote(match.group(1)), urllib.parse.unquote(match.group(2))) == ("hmailserver", version),
                   "the link of %s is a file of hmailserver %s that the feed accepts" % (file_name, version))
    expect(permalink_name("hMailServer-6.3.5-x64.exe", version) == "hMailServer-x64.exe", "the installer's permanent name")
    expect(permalink_name("hmailserver_6.3.5_amd64.deb", version) == "hmailserver_amd64.deb", "the .deb's permanent name")
    expect(permalink_name("hmailserver-6.3.5-1.x86_64.rpm", version) == "hmailserver.x86_64.rpm", "the .rpm's permanent name")
    expect(permalink_name("hmailserver-linux-6.3.5-SHA256SUMS.txt", version) == "hmailserver-linux-SHA256SUMS.txt", "the sums' permanent name")
    expect(permalink_name("hMailServer-6.3.5-x64.exe.cosign.bundle", version) == "hMailServer-6.3.5-x64.exe.cosign.bundle", "a bundle keeps its name")
    expect(permalink_name("hmailserver.spdx.json", version) == "hmailserver.spdx.json", "an SBOM keeps its name")
    expect(link_type("hmailserver_6.3.5_amd64.deb") == "package" and link_type("hmailserver.spdx.json") == "other"
           and link_type("hMailServer-6.3.5-x64.exe.cosign.bundle") == "other", "packages are packages, the rest is other")
    for bad in ("6.3.5", "v", "build-inputs-1", "vx.1"):
        try:
            version_of(bad)
            failures.append("%r is refused as a release tag" % bad)
        except ReleaseError:
            pass
    expect(version_of("v6.3.5-rc1") == "6.3.5-rc1", "a pre-release's tag names its version")
    # The files 6.3.6 adds, and their permanent paths: a distribution release's name
    # keeps its release, the ~ of a .deb's version never reaches a path, and a bundle
    # and its copy keep their names.
    for name, path in (("hmailserver_6.3.5-1~debian12_amd64.deb", "hmailserver_debian12_amd64.deb"),
                       ("hmailserver_6.3.5-1~ubuntu24.04_arm64.deb.cdx.json", "hmailserver_ubuntu24.04_arm64.deb.cdx.json"),
                       ("hmailserver_6.3.5-1~debian13_arm64.deb.cosign.bundle", "hmailserver_6.3.5-1-debian13_arm64.deb.cosign.bundle"),
                       ("hmailserver_6.3.5-1~debian13_arm64.deb.sigstore.json", "hmailserver_6.3.5-1-debian13_arm64.deb.sigstore.json"),
                       ("hmailserver-6.3.5-1.fc44.x86_64.rpm", "hmailserver.fc44.x86_64.rpm"),
                       ("hmailserver-6.3.5-1.suse.lp160.aarch64.rpm", "hmailserver.suse.lp160.aarch64.rpm"),
                       ("hMailServer-6.3.5-x86_64.AppImage", "hMailServer-x86_64.AppImage"),
                       ("hMailServer-6.3.5-aarch64.AppImage.cdx.json", "hMailServer-aarch64.AppImage.cdx.json"),
                       ("hmailserver-6.3.5.tar.gz", "hmailserver.tar.gz"), ("hmailserver-chart-6.3.5.tgz", "hmailserver-chart.tgz"),
                       ("hmailserver-6.3.5.intoto.sigstore.json", "hmailserver.intoto.sigstore.json"),
                       ("hmailserver.spdx.json.sigstore.json", "hmailserver.spdx.json.sigstore.json"),
                       ("hMailServer-6.3.5-x64.exe.sigstore.json", "hMailServer-6.3.5-x64.exe.sigstore.json")):
        expect(permalink_name(name, version) == path, "%s's permanent name is %s: %s" % (name, path, permalink_name(name, version)))
    expect(link_type("hMailServer-6.3.5-x86_64.AppImage") == "package" and link_type("hmailserver-6.3.5.tar.gz") == "other"
           and link_type("hmailserver_6.3.5_amd64.deb.sigstore.json") == "other" and link_type(provenance_name(version)) == "other",
           "an AppImage is a package; the tarball, a bundle's copy and the provenance are other")
    every_link = release_links(version, release_files(version))
    expect(link_problems(every_link) == [], "every link of a release is one GitLab takes: %s" % link_problems(every_link))
    for what, links, needle in (
            ("a path with the ~ of a .deb's version", [{"name": "a~b.deb", "url": "u1", "direct_asset_path": "/downloads/a~b.deb"}], "which GitLab refuses"),
            ("a path over 128 characters", [{"name": "a", "url": "u1", "direct_asset_path": "/downloads/" + "a" * 118}], "which GitLab refuses"),
            ("a path ending in a dot", [{"name": "a", "url": "u1", "direct_asset_path": "/downloads/a."}], "which GitLab refuses"),
            ("two links of one path", [{"name": "a", "url": "u1", "direct_asset_path": "/downloads/a"},
                                       {"name": "b", "url": "u2", "direct_asset_path": "/downloads/a"}], "direct asset path"),
            ("two links of one name", [{"name": "a", "url": "u1", "direct_asset_path": "/downloads/a"},
                                       {"name": "a", "url": "u2", "direct_asset_path": "/downloads/b"}], "the name a"),
            ("a file name the registry refuses", [{"name": "~a", "url": "u1", "direct_asset_path": "/downloads/a"}], "registry takes")):
        found = link_problems(links)
        expect(any(needle in p for p in found), "%s is refused: %s" % (what, found))
    expect(any("GitLab takes" in p for p in link_problems([], "x" * (GITLAB_DESCRIPTION_MAX + 1))), "notes over a million characters are refused")
    held_safe = globals()["link_safe"]
    globals()["link_safe"] = lambda name: name
    try:
        expect(any("hmailserver_6.3.5-1~debian12_amd64.deb" in p for p in link_problems(release_links(version, release_files(version)))),
               "without link_safe, a distribution release's .deb would carry its ~ into a path GitLab refuses")
    finally:
        globals()["link_safe"] = held_safe

    changelog = ("# Changelog\n\n## Unreleased - in the tree after 6.3.5\n\nNothing yet.\n\n---\n\n"
                 "## [6.3.5](https://gitlab.com/x/-/releases/v6.3.5) - 2026-10-10\n\nThe notes.\n\n### A part\n\nMore.\n\n---\n\n"
                 "## [6.3.4](https://gitlab.com/x/-/releases/v6.3.4) - 2026-09-26\n\nOlder.\n")
    section = changelog_section(changelog, "6.3.5")
    expect(section.startswith("The notes.") and "### A part" in section and "Older." not in section, "the version's section, and no other: %r" % section)
    try:
        changelog_section(changelog, "6.3.6")
        failures.append("a version with no section in CHANGELOG.md is refused")
    except ReleaseError:
        pass

    # The allow-list's keys and their fingerprints. KEY_A is the owner's, from
    # .gitlab/allowed_signers, with the fingerprint ssh-keygen -l prints for it.
    key_a = "AAAAC3NzaC1lZDI1NTE5AAAAIIB9HiI7F4JgXDg9DyvL3KSQUlKfygNjVkT2rKf6pOUL"
    key_b = base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519\x00\x00\x00\x20" + bytes(range(32))).decode("ascii")
    list_a = "# the owner's\nowner@example.org ssh-ed25519 %s\n" % key_a
    list_b = "someone@example.org ssh-ed25519 %s\n" % key_b
    expect(fingerprint(key_a) == "SHA256:bz6MOtUcFMhhzlNiUhIZL0BzitPE875hMk1Yon5huAA", "a key's fingerprint is ssh-keygen's")
    expect(fingerprint("not base64!") == "" and fingerprint("") == "", "a key that is missing or not base64 has no fingerprint")
    expect(signer_keys('# c\n\na@b.c,d@e.f namespaces="git" ssh-ed25519 %s comment\n' % key_a) == [("a@b.c,d@e.f", "ssh-ed25519", key_a)],
           "an allowed_signers line with options is read as its principals and its key")
    expect(len(signer_keys(list_a + list_b)) == 2 and signer_keys("# only a comment\n") == [], "every key line is counted, no comment")

    with tempfile.TemporaryDirectory() as work:
        # A checkout with the jobs' artifacts in it, as `release` sees one.
        root = os.path.join(work, "checkout")

        def write(relative, data):
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            return path

        write(VERSION_H, b'#pragma once\n#define HMAILSERVER_VERSION "6.3.5"\n#define HMAILSERVER_VERSION_NUMERIC 6,3,5,44\n')
        for name, relative in sources(version).items():
            write(relative, ("the bytes of %s\n" % name).encode("utf-8"))

        def sbom_of(path, kind):
            """A document this project's build/asset-sbom.py would accept as describing path."""
            sha256, sha512, size = asset_sbom.digests(path)
            subject = asset_sbom.subject(os.path.basename(path), kind, version, sha256, sha512, size, "c" * 40)
            return json.dumps(asset_sbom.document(subject, [], sha256, kind)).encode("utf-8")

        for key, kind in (("deb-amd64", "deb"), ("rpm-x86_64", "rpm"), ("deb-arm64", "deb"), ("rpm-aarch64", "rpm")):
            write(sources(version)[sbom_name(names[key])], sbom_of(os.path.join(root, sources(version)[names[key]]), kind))
        deb = os.path.join(root, sources(version)[names["deb-amd64"]])
        rpm = os.path.join(root, sources(version)[names["rpm-x86_64"]])
        sums = "%s  %s\n%s  %s\n" % (sha256_of(deb), names["deb-amd64"], sha256_of(rpm), names["rpm-x86_64"])
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))
        # The package jobs' own sums, as deb-build.sh and rpm-build.sh write them: the
        # .deb by its name, and the .fc44 package and its generic copy.
        deb_arm64 = os.path.join(root, sources(version)[names["deb-arm64"]])
        rpm_aarch64 = os.path.join(root, sources(version)[names["rpm-aarch64"]])
        deb_job_sums = "%s  %s\n" % (sha256_of(deb_arm64), names["deb-arm64"])
        rpm_job_sums = "%s  hmailserver-6.3.5-1.fc44.aarch64.rpm\n%s  generic/%s\n" % ("e" * 64, sha256_of(rpm_aarch64), names["rpm-aarch64"])
        write(package_job_sums(version)[names["deb-arm64"]][0], deb_job_sums.encode("utf-8"))
        write(package_job_sums(version)[names["rpm-aarch64"]][0], rpm_job_sums.encode("utf-8"))
        # What release-packages gathered: each distribution release's package, the
        # AppImages and the tarball, each with its SBOM and its build job's sums as
        # that job wrote them; and the chart with helm-chart-package's.
        gathered_kinds = dict((b["name"], b["kind"]) for b in gathered_builds(version) if b["sbom"])
        for name, kind in gathered_kinds.items():
            write(sources(version)[sbom_name(name)], sbom_of(os.path.join(root, sources(version)[name]), kind))
        for name, (relative, listed) in package_job_sums(version).items():
            if name not in (names["deb-arm64"], names["rpm-aarch64"]):
                write(relative, ("%s  %s\n" % (sha256_of(os.path.join(root, sources(version)[name])), listed)).encode("utf-8"))
        # The tagged tree's own allow-list names another key than master's: a tag
        # signed with it must be refused, which it is only if master's is read.
        write(ALLOWED_SIGNERS, list_b.encode("utf-8"))
        # The four scans' artifacts as a pipeline that found nothing leaves them, and an
        # exceptions file that accepts nothing.
        for relative, content in security_gate.sample_reports().items():
            write(relative, content.encode("utf-8"))
        write(security_gate.EXCEPTIONS, b"exceptions: []\n")

        built = "c" * 40
        env = {"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:" + "a" * 64, "CI_COMMIT_SHA": built}
        for name in AZURE_SETTINGS:
            env[name] = "set"

        class Done(object):
            def __init__(self, code, out="", err=""):
                self.returncode, self.stdout, self.stderr = code, out, err

        def fake_git(commit=built, on_master=True, master_list=list_a, signed_by=key_a, fetch_master=True, names_key=None,
                     hidden=False, unshallow=True, master_exceptions="exceptions: []\n"):
            """git as the release job's checkout answers it. master's history and its
            allow-list are there only once master has been fetched by name; with
            hidden, another shallow boundary hides the way down to the tagged commit
            until master's whole history is fetched. verify-tag reads whatever
            allow-list it is given, and a signature is good when that list holds the
            key the tag was signed with."""
            state = {"master": False, "whole": False}

            def answer(arguments, **kwargs):
                if "cat-file" in arguments:
                    return Done(0, "tag\n")
                if "--is-shallow-repository" in arguments:
                    return Done(0, "false\n" if state["whole"] else "true\n")
                if "rev-parse" in arguments:
                    return Done(0, commit + "\n")
                if "fetch" in arguments:
                    if "+refs/heads/%s:%s" % (TRUSTED_BRANCH, TRUSTED_REF) in arguments:
                        if not fetch_master or ("--unshallow" in arguments and not unshallow):
                            return Done(128, "", "fatal: could not read from remote repository")
                        state["master"] = True
                        state["whole"] = state["whole"] or "--unshallow" in arguments
                    return Done(0)
                if "merge-base" in arguments:
                    seen = state["master"] and (state["whole"] or not hidden)
                    return Done(0 if on_master and seen and TRUSTED_REF in arguments else 1)
                if "show" in arguments:
                    if state["master"] and arguments[-1] == "%s:%s" % (TRUSTED_REF, ALLOWED_SIGNERS) and master_list is not None:
                        return Done(0, master_list)
                    if state["master"] and arguments[-1] == "%s:%s" % (TRUSTED_REF, security_gate.EXCEPTIONS) and master_exceptions is not None:
                        return Done(0, master_exceptions)
                    return Done(128, "", "fatal: path '%s' does not exist" % ALLOWED_SIGNERS)
                if "verify-tag" in arguments:
                    given = [a.split("=", 1)[1] for a in arguments if a.startswith("gpg.ssh.allowedSignersFile=")]
                    path = given[0] if given else ""
                    path = path if os.path.isabs(path) else os.path.join(root, path)
                    try:
                        with open(path, encoding="utf-8") as f:
                            listed = signer_keys(f.read())
                    except OSError:
                        return Done(1, "", "error: no allowed signers file")
                    for principal, _, key in listed:
                        if key == signed_by:
                            shown = names_key or key
                            return Done(0, "", 'Good "git" signature for %s with ED25519 key %s\n' % (principal, fingerprint(shown)))
                    return Done(1, "", "Could not verify signature.\nno principal matched")
                return Done(0)
            return answer

        class FakeGitLab(object):
            def __init__(self):
                self.files = {}
                self.releases = []
                self.calls = []
                self.next_id = [100]
                self.milestones = []
                self.posted = []

            def seed(self, name, data):
                """A file the registry holds before this run, as an earlier one or someone else left it."""
                self.next_id[0] += 1
                self.files.setdefault((PACKAGE, version), []).append({"id": self.next_id[0], "file_name": name, "size": len(data),
                                                                      "file_sha256": hashlib.sha256(data).hexdigest()})
                return self

            def request(self, method, url, body=None, content_type=None):
                self.calls.append((method, url.split("?")[0]))
                parsed = urllib.parse.urlparse(url)
                path = parsed.path[len("/api/v4/projects/" + PROJECT_ID):]
                query = urllib.parse.parse_qs(parsed.query)
                if method == "PUT":
                    m = re.match(r"^/packages/generic/([^/]+)/([^/]+)/([^/]+)$", path)
                    self.next_id[0] += 1
                    key = (urllib.parse.unquote(m.group(1)), urllib.parse.unquote(m.group(2)))
                    self.files.setdefault(key, []).append({"id": self.next_id[0], "file_name": urllib.parse.unquote(m.group(3)),
                                                           "size": len(body), "file_sha256": hashlib.sha256(body).hexdigest()})
                    return 201, b"{}", {}
                if method == "POST" and path == "/releases":
                    value = json.loads(body.decode("utf-8"))
                    self.posted.append(value)
                    # As GitLab's Releases API: a milestone is named by its exact
                    # title, and one the project does not have refuses the release.
                    missing = [t for t in value.get("milestones", []) if t not in self.milestones]
                    if missing:
                        return 400, json.dumps({"message": "Milestone(s) not found: " + ", ".join(missing)}).encode("utf-8"), {}
                    self.releases.insert(0, {"tag_name": value["tag_name"], "name": value["name"], "upcoming_release": False,
                                             "released_at": "2026-10-10T10:00:00.000Z", "description": value["description"],
                                             "_links": {"self": feed.PROJECT_URL + "/-/releases/" + value["tag_name"]},
                                             "assets": {"links": value["assets"]["links"]}})
                    return 201, b"{}", {}
                if method == "GET":
                    m = re.match(r"^/releases/([^/]+)$", path)
                    if m:
                        found = [r for r in self.releases if r["tag_name"] == urllib.parse.unquote(m.group(1))]
                        return (200, json.dumps(found[0]).encode("utf-8"), {}) if found else (404, b'{"message":"404 Not Found"}', {})
                    if path == "/milestones":
                        # A job's token may not use the Milestones API.
                        return 401, b'{"message":"401 Unauthorized"}', {}
                    page = int(query.get("page", ["1"])[0])
                    if path == "/releases":
                        items = self.releases
                    elif path == "/packages":
                        items = [{"id": i, "name": k[0], "version": k[1]} for i, k in enumerate(sorted(self.files))]
                    else:
                        m = re.match(r"^/packages/(\d+)/package_files$", path)
                        items = self.files[sorted(self.files)[int(m.group(1))]]
                    # GitLab's pages of a hundred at most, the next named in X-Next-Page: a
                    # release is more than a hundred files from 6.3.6.
                    more = str(page + 1) if len(items) > page * 100 else ""
                    return 200, json.dumps(items[(page - 1) * 100:page * 100]).encode("utf-8"), {"x-next-page": more}
                return 400, b'{"message":"unexpected"}', {}

            def feed_get(self, url):
                status, body, headers = self.request("GET", url)
                if status != 200:
                    raise feed.FeedError("HTTP %d" % status)
                return body, headers

        signed_tag = fake_git()

        out = os.path.join(work, "release-assets")
        problems = collect(root, tag, out, env, run=signed_tag, api=FakeGitLab())
        expect(problems == [], "a complete release, its tag on master and signed by master's one key, is collected: %s" % problems)
        gathered = sorted(os.listdir(out)) if os.path.isdir(out) else []
        expect(gathered == sorted(collected_assets(version)), "under the names a release carries: %s" % (gathered or "nothing"))
        expect(sorted(set(expected_assets(version)) - set(collected_assets(version))) == [sbom_name(names["windows"])],
               "everything but the installer's SBOM, which release-sbom makes from the signed installer")
        expect(all(sbom_name(n) in expected_assets(version) for n in compiled_assets(version)),
               "every compiled file goes out with its SBOM")
        expect(names["deb-arm64"] in gathered and names["rpm-aarch64"] in gathered
               and sbom_name(names["deb-arm64"]) in gathered and sbom_name(names["rpm-aarch64"]) in gathered,
               "the AArch64 .deb and .rpm are collected with their SBOMs: %s" % gathered)
        # Every file 6.3.6 carries, by name: the five of 6.3.5's chain, each distribution
        # release's .deb and .rpm, the AppImages, the source of their runtime's libraries,
        # the tarball and the chart; each compiled one and the tarball with its SBOM, every
        # Linux one with a line in the sums.
        every_deb = ["hmailserver_6.3.5-1~%s_%s.deb" % (d, a) for d in ("debian12", "debian13", "ubuntu24.04") for a in ("amd64", "arm64")]
        every_rpm = ["hmailserver-6.3.5-1.%s.%s.rpm" % (t, a) for t in ("fc44", "fc43", "el10", "el9", "suse.lp160", "suse.tw")
                     for a in ("x86_64", "aarch64")]
        more = every_deb + every_rpm + ["hMailServer-6.3.5-x86_64.AppImage", "hMailServer-6.3.5-aarch64.AppImage", "hmailserver-6.3.5.tar.gz"]
        runtime_sources = "hmailserver-6.3.5-appimage-runtime-sources.tar.gz"
        primaries = [names["windows"], names["deb-amd64"], names["rpm-x86_64"], names["deb-arm64"], names["rpm-aarch64"]] + more + [
            runtime_sources, "hmailserver-chart-6.3.5.tgz", sums_name(version), "hmailserver.spdx.json", "hmailserver.cyclonedx.json"]
        with_sbom = primaries[:5] + more
        expect(len(every_deb) == 6 and len(every_rpm) == 12 and sorted(primary_assets(version)) == sorted(primaries),
               "the primary files are the installer, the four generic packages, the 18 distribution releases' packages, the "
               "two AppImages, the source of their runtime's libraries, the tarball, the chart, the sums and the two tree "
               "SBOMs: %s" % sorted(set(primary_assets(version)) ^ set(primaries)))
        expect(runtime_sources_name(version) == runtime_sources and runtime_sources not in compiled_assets(version)
               and sbom_name(runtime_sources) not in expected_assets(version),
               "the runtime's sources go out as source, without an SBOM: %s" % runtime_sources_name(version))
        expect(sorted(expected_assets(version)) == sorted(primaries + [n + ".cdx.json" for n in with_sbom]) and len(expected_assets(version)) == 57,
               "every compiled file and the tarball go out with <file>.cdx.json, the runtime's sources and the chart without, "
               "57 files in all: %d" % len(expected_assets(version)))
        expect(sorted(release_files(version)) == sorted(expected_assets(version) + [n + ".cosign.bundle" for n in expected_assets(version)]
                                                        + [n + ".sigstore.json" for n in primaries] + ["hmailserver-6.3.5.intoto.sigstore.json"])
               and len(release_files(version)) == 146,
               "a release is those 57, a bundle beside each, a .sigstore.json copy of each of the 31 primary files' bundles, "
               "and the provenance: 146 files and links: %d" % len(release_files(version)))
        expect(sorted(gathered) == sorted(set(expected_assets(version)) - {names["windows"] + ".cdx.json"}),
               "collect gathers every one of them but the installer's SBOM: %s" % sorted(set(expected_assets(version)) - set(gathered)))
        released_sums = b""
        if os.path.isfile(os.path.join(out, sums_name(version))):
            with open(os.path.join(out, sums_name(version)), "rb") as f:
                released_sums = f.read()
        members = [names["deb-arm64"], names["rpm-aarch64"]] + more[:-1] + [runtime_sources, more[-1], "hmailserver-chart-6.3.5.tgz"]
        expect(released_sums == (sums + "".join("%s  %s\n" % (sha256_of(os.path.join(root, sources(version)[n])), n) for n in members)).encode("ascii"),
               "the release's sums are linux-build's two lines as they are, then the AArch64 .deb's and .rpm's, then a "
               "line for every other Linux file and the chart: %r" % released_sums)
        listed = set(line.split()[1] for line in released_sums.decode("ascii").splitlines())
        expect(listed == set(n for n in primaries if n not in (names["windows"], sums_name(version), "hmailserver.spdx.json",
                                                               "hmailserver.cyclonedx.json")),
               "every primary file but the installer, which Authenticode changes after collect, and the sums and tree "
               "SBOMs has a line in the sums: %s" % sorted(listed))
        if not gathered:
            # So that publishing is still tested when collecting failed.
            os.makedirs(out, exist_ok=True)
            for name in collected_assets(version):
                shutil.copyfile(os.path.join(root, sources(version)[name]), os.path.join(out, name))

        def refused(what, needle, **changes):
            e = dict(env)
            e.update(changes.get("env", {}))
            run = changes.get("run", signed_tag)
            api = changes["api"] if "api" in changes else FakeGitLab()
            found = collect(root, changes.get("tag", tag), os.path.join(work, "refused"), e, run=run, api=api)
            expect(any(needle in p for p in found), "%s is refused: %s" % (what, found))
            expect(not os.path.isdir(os.path.join(work, "refused")), "%s copies nothing" % what)

        refused("a tag Version.h does not name", "Version.h says 6.3.5", tag="v6.3.6")
        refused("a cosign image left unpinned", "HM_COSIGN_IMAGE", env={"HM_COSIGN_IMAGE": "gcr.io/projectsigstore/cosign:v2.6.1-dev@sha256:PIN-THE-DIGEST"})
        refused("a cosign image from ghcr.io, pinned, as a CI/CD variable could set it", "not gcr.io/projectsigstore/cosign",
                env={"HM_COSIGN_IMAGE": "ghcr.io/sigstore/cosign/cosign:v2.6.1-dev@sha256:" + "a" * 64})
        refused("a tag moved to another commit after the pipeline started", "the tag was moved", run=fake_git(commit="d" * 40))
        refused("a collect outside a pipeline", "CI_COMMIT_SHA is", env={"CI_COMMIT_SHA": ""})
        refused("an Azure setting left out", "ARTIFACT_SIGNING_PROFILE", env={"ARTIFACT_SIGNING_PROFILE": " "})
        refused("a lightweight tag", "is not an annotated tag", run=lambda a, **k: Done(0, "commit\n"))
        # The tag's signer, against master's allow-list and not the tagged tree's.
        refused("a tag on a commit that is not on master", "which is not on master", run=fake_git(on_master=False))
        refused("a tag not on master, asked again with master's whole history", "which is not on master",
                run=fake_git(on_master=False, hidden=True))
        refused("a checkout that cannot fetch master's whole history when a shallow boundary hides the way down",
                "whole history could not be fetched", run=fake_git(hidden=True, unshallow=False))
        problems = collect(root, tag, os.path.join(work, "deepened"), env, run=fake_git(hidden=True), api=FakeGitLab())
        expect(problems == [], "a tag on master whose way down a shallow boundary hides is found with master's whole history: %s" % problems)
        refused("a tag signed by the key its own tree lists and master does not", "is not signed by the key master's",
                run=fake_git(signed_by=key_b))
        refused("a tag signed by a second key master's list gained", "lists 2 key(s)",
                run=fake_git(master_list=list_a + list_b, signed_by=key_b))
        refused("master's list of two keys, even for the owner's", "lists 2 key(s)", run=fake_git(master_list=list_a + list_b))
        refused("master's list with no key", "lists 0 key(s)", run=fake_git(master_list="# nobody\n"))
        refused("no allow-list on master", "master has no .gitlab/allowed_signers", run=fake_git(master_list=None))
        refused("master not fetched", "master could not be fetched", run=fake_git(fetch_master=False))
        refused("git naming another key than the listed one", "not by the one key master's", run=fake_git(names_key=key_b))
        # The registry, before anything is signed.
        refused("a collect without the job's token", "there is no CI_JOB_TOKEN", api=None)
        refused("a .deb the registry holds with other bytes", "already holds %s with the SHA-256" % names["deb-amd64"],
                api=FakeGitLab().seed(names["deb-amd64"], b"other bytes"))
        refused("an installer an earlier run's signing left in the registry", "already holds %s, which an earlier run's signing made" % names["windows"],
                api=FakeGitLab().seed(names["windows"], ("the bytes of %s\n" % names["windows"]).encode("utf-8")))
        refused("a bundle an earlier run left in the registry", "hmailserver.spdx.json.cosign.bundle, which an earlier run's signing made",
                api=FakeGitLab().seed("hmailserver.spdx.json.cosign.bundle", b"{}"))
        refused("an installer SBOM an earlier run left in the registry", "%s, which an earlier run's signing made" % sbom_name(names["windows"]),
                api=FakeGitLab().seed(sbom_name(names["windows"]), b"{}"))
        refused("a bundle's .sigstore.json copy an earlier run left in the registry",
                "%s.sigstore.json, which an earlier run's signing made" % names["deb-arm64"],
                api=FakeGitLab().seed(names["deb-arm64"] + ".sigstore.json", b"{}"))
        refused("a provenance an earlier run left in the registry", "%s, which an earlier run's signing made" % provenance_name(version),
                api=FakeGitLab().seed(provenance_name(version), b"{}"))
        held = FakeGitLab()
        for name in collected_assets(version):
            if name != names["windows"]:
                # As release-publish sent them: collect's files, the sums as collect wrote them.
                with open(os.path.join(out, name), "rb") as f:
                    held.seed(name, f.read())
        problems = collect(root, tag, os.path.join(work, "again"), env, run=signed_tag, api=held)
        expect(problems == [], "files the registry holds with the same bytes, from a publish that stopped part-way, are collected again: %s" % problems)
        write(sources(version)[sums_name(version)], ("%s  %s\n" % ("0" * 64, names["deb-amd64"])).encode("utf-8"))
        refused("a sums file that is not the packages'", "is not the sums of the two x86-64 packages")
        write(sources(version)[sums_name(version)], sums.encode("utf-8"))
        deb_sbom = os.path.join(root, sources(version)[sbom_name(names["deb-amd64"])])
        with open(deb_sbom, "rb") as f:
            good_sbom = f.read()
        write(sources(version)[sbom_name(names["deb-amd64"])], sbom_of(os.path.join(root, sources(version)[names["rpm-x86_64"]]), "deb"))
        refused("a .deb whose SBOM describes another file", "%s does not describe %s" % (sbom_name(names["deb-amd64"]), names["deb-amd64"]))
        write(sources(version)[sbom_name(names["deb-amd64"])], b"not json")
        refused("a .deb whose SBOM is not JSON", "%s does not describe" % sbom_name(names["deb-amd64"]))
        os.remove(deb_sbom)
        refused("a .deb without its SBOM", "%s is not in this job's artifacts" % sbom_name(names["deb-amd64"]))
        write(sources(version)[sbom_name(names["deb-amd64"])], good_sbom)
        # The AArch64 packages: held to their build jobs' sums, and each to its SBOM job's SBOM.
        deb_sums_path = package_job_sums(version)[names["deb-arm64"]][0]
        rpm_sums_path = package_job_sums(version)[names["rpm-aarch64"]][0]
        write(deb_sums_path, ("%s  %s\n" % ("0" * 64, names["deb-arm64"])).encode("utf-8"))
        refused("an arm64 .deb that is not the bytes its build job made", "gives %s the SHA-256 %s" % (names["deb-arm64"], "0" * 64))
        write(deb_sums_path, ("%s  hmailserver_6.3.5_amd64.deb\n" % sha256_of(deb_arm64)).encode("utf-8"))
        refused("an arm64 .deb its build job's sums do not name", "does not name %s" % names["deb-arm64"])
        os.remove(os.path.join(root, deb_sums_path))
        refused("an arm64 .deb without its build job's sums", "the sums of the build that made %s" % names["deb-arm64"])
        write(deb_sums_path, deb_job_sums.encode("utf-8"))
        write(rpm_sums_path, ("%s  %s\n" % (sha256_of(rpm_aarch64), names["rpm-aarch64"])).encode("utf-8"))
        refused("an aarch64 .rpm named in its build job's sums without its place under generic/", "does not name generic/%s" % names["rpm-aarch64"])
        write(rpm_sums_path, rpm_job_sums.encode("utf-8"))
        arm_sbom = os.path.join(root, sources(version)[sbom_name(names["deb-arm64"])])
        with open(arm_sbom, "rb") as f:
            good_arm_sbom = f.read()
        write(sources(version)[sbom_name(names["deb-arm64"])], sbom_of(deb, "deb"))
        refused("an arm64 .deb whose SBOM describes the amd64 one", "%s does not describe %s" % (sbom_name(names["deb-arm64"]), names["deb-arm64"]))
        write(sources(version)[sbom_name(names["deb-arm64"])], good_arm_sbom)
        os.remove(os.path.join(root, sources(version)[sbom_name(names["rpm-aarch64"])]))
        refused("an aarch64 .rpm without its SBOM", "%s is not in this job's artifacts" % sbom_name(names["rpm-aarch64"]))
        write(sources(version)[sbom_name(names["rpm-aarch64"])], sbom_of(rpm_aarch64, "rpm"))
        os.remove(rpm_aarch64)
        refused("a release without the aarch64 .rpm", "%s is not in this job's artifacts (%s)" % (names["rpm-aarch64"], sources(version)[names["rpm-aarch64"]]))
        write(sources(version)[names["rpm-aarch64"]], ("the bytes of %s\n" % names["rpm-aarch64"]).encode("utf-8"))
        problems = collect(root, tag, os.path.join(work, "arm-again"), env, run=signed_tag, api=FakeGitLab())
        expect(problems == [], "the AArch64 files put back, the release is collected again: %s" % problems)
        refused("release sums the registry holds with other bytes", "already holds %s with the SHA-256" % sums_name(version),
                api=FakeGitLab().seed(sums_name(version), sums.encode("utf-8")))
        os.remove(os.path.join(root, sources(version)["hmailserver.spdx.json"]))
        refused("a missing SBOM", "hmailserver.spdx.json is not in this job's artifacts")
        write(sources(version)["hmailserver.spdx.json"], b"the bytes of hmailserver.spdx.json\n")
        # The security scans: a report missing, or a scan that failed, holds the release.
        samples = security_gate.sample_reports()
        os.remove(os.path.join(root, security_gate.GITLAB_SAST_REPORT))
        refused("a release whose semgrep-sast report is missing", "semgrep-sast: there is no")
        write(security_gate.GITLAB_SAST_REPORT, samples[security_gate.GITLAB_SAST_REPORT].encode("utf-8"))
        failed = json.loads(samples[security_gate.DEPENDENCY_REPORTS[1]])
        failed["scan"]["status"] = "failure"
        write(security_gate.DEPENDENCY_REPORTS[1], json.dumps(failed).encode("utf-8"))
        refused("a release whose dependency scan could not run", "not success")
        write(security_gate.DEPENDENCY_REPORTS[1], samples[security_gate.DEPENDENCY_REPORTS[1]].encode("utf-8"))
        refused("a release with an exceptions file the gate cannot read", "it has no finding",
                run=fake_git(master_exceptions="exceptions:\n  - scan: semgrep\n"))
        refused("master without an exceptions file", "master's .gitlab/security-exceptions.yml could not be read",
                run=fake_git(master_exceptions=None))
        # A finding that holds the release, accepted by the tagged tree's own copy and not
        # by master's: master's is the one read.
        advisory = json.loads(samples[security_gate.DEPENDENCY_REPORTS[0]])
        advisory["vulnerabilities"] = [{"id": "u", "name": "CVE-2026-0001 in zlib 1.3", "severity": "Critical",
                                        "identifiers": [{"type": "cve", "name": "CVE-2026-0001", "value": "CVE-2026-0001"}],
                                        "location": {"file": "lock", "dependency": {"package": {"name": "zlib"}, "version": "1.3"}},
                                        "solution": "Upgrade zlib to 1.3.1."}]
        write(security_gate.DEPENDENCY_REPORTS[0], json.dumps(advisory).encode("utf-8"))
        # Reviewed today (UTC, as the gate judges), in force for half a year.
        clock = security_gate.datetime
        reviewed = clock.datetime.now(clock.timezone.utc).date()
        accepted = ("exceptions:\n  - scan: dependency-scan\n    finding: CVE-2026-0001\n    package: zlib\n    what: zlib\n"
                    "    reason: 'the vulnerable inflate path is not reached: the server only deflates'\n"
                    "    reviewed-by: Progressiverobot\n    reviewed: %s\n    expires: %s\n" % (
                        reviewed.isoformat(), (reviewed + clock.timedelta(days=180)).isoformat()))
        write(security_gate.EXCEPTIONS, accepted.encode("utf-8"))
        refused("a finding the tagged tree accepts and master does not", "CVE-2026-0001 in zlib")
        write(security_gate.EXCEPTIONS, b"exceptions: []\n")
        problems = collect(root, tag, os.path.join(work, "accepted"), env, run=fake_git(master_exceptions=accepted), api=FakeGitLab())
        expect(problems == [], "a finding master's exceptions accept is collected: %s" % problems)
        write(security_gate.DEPENDENCY_REPORTS[0], samples[security_gate.DEPENDENCY_REPORTS[0]].encode("utf-8"))
        problems = collect(root, tag, os.path.join(work, "scanned"), env, run=signed_tag, api=FakeGitLab())
        expect(problems == [], "the scans put back, the release is collected again: %s" % problems)

        # Each file 6.3.6 adds is held as the AArch64 pair is: missing, unlike the sums its
        # job wrote, or with an SBOM that describes another file, the release is refused.
        tarball_check = {"code": 0}

        def with_tarball_check(arguments, **kwargs):
            if any(str(a).endswith("check-source-tarball.py") for a in arguments):
                return Done(tarball_check["code"], "" if tarball_check["code"] == 0 else "PROBLEM: the pax header names another commit", "")
            return signed_tag(arguments, **kwargs)

        def swap(relative, data):
            path = os.path.join(root, relative)
            with open(path, "rb") as f:
                kept = f.read()
            if data is None:
                os.remove(path)
            else:
                with open(path, "wb") as f:
                    f.write(data)
            return lambda: write(relative, kept)

        added = [b["name"] for b in gathered_builds(version)] + [chart_name(version)]
        for name in added:
            put_back = swap(sources(version)[name], None)
            refused("a release without %s" % name, "%s is not in this job's artifacts" % name, run=with_tarball_check)
            put_back()
            if name in package_job_sums(version):
                relative, listed = package_job_sums(version)[name]
                put_back = swap(relative, ("%s  %s\n" % ("0" * 64, listed)).encode("utf-8"))
                refused("%s unlike the sums its job wrote" % name, "the file is not the one its build job made", run=with_tarball_check)
                put_back()
            if name in compiled_assets(version):
                other = os.path.join(root, sources(version)[names["deb-amd64"]])
                put_back = swap(sources(version)[sbom_name(name)], sbom_of(other, "deb"))
                refused("%s with an SBOM of another file" % name, "%s does not describe %s" % (sbom_name(name), name), run=with_tarball_check)
                put_back()
                put_back = swap(sources(version)[sbom_name(name)], None)
                refused("%s without its SBOM" % name, "%s is not in this job's artifacts" % sbom_name(name), run=with_tarball_check)
                put_back()
        tarball_check["code"] = 1
        refused("a tarball build/check-source-tarball.py refuses", "is not the source tarball of", run=with_tarball_check)
        tarball_check["code"] = 0
        problems = collect(root, tag, os.path.join(work, "all-back"), env, run=with_tarball_check, api=FakeGitLab())
        expect(problems == [], "every file put back, the release is collected again: %s" % problems)

        # The rehearsal: a protected batch* branch, Version.h's version, the installer
        # windows-packages-x64 built under its commit-stamped name, and nothing asked of
        # git, the tag, the scans or the registry.
        branch_installer = rehearsal_installer(version, built)
        expect(branch_installer == "hMailServer-6.3.5-cccccccc-x64.exe", "the branch's installer is named with its commit: %s" % branch_installer)
        write(REHEARSAL_INSTALLER_DIR + "/" + branch_installer, b"MZ the branch's installer")
        write(REHEARSAL_INSTALLER_DIR + "/SHA256SUMS", ("%s  %s\r\n" % (hashlib.sha256(b"MZ the branch's installer").hexdigest(), branch_installer)).encode("ascii"))
        rehearsal_env = {"HM_COSIGN_IMAGE": env["HM_COSIGN_IMAGE"], "CI_COMMIT_SHA": built, "CI_COMMIT_BRANCH": "batch42",
                         "CI_COMMIT_REF_PROTECTED": "true", "CI_DEFAULT_BRANCH": "master", "CI_PROJECT_PATH": "Progressiverobot/hmailserver"}
        asked = []

        def no_git(arguments, **kwargs):
            asked.append(arguments)
            if any(str(a).endswith("check-source-tarball.py") for a in arguments):
                return Done(0)
            return Done(128, "", "fatal: a rehearsal asks git nothing")

        rehearsed = os.path.join(work, "rehearsal-assets")
        problems = collect(root, None, rehearsed, rehearsal_env, run=no_git, api=None, rehearse=True)
        expect(problems == [], "a rehearsal on a protected batch* branch is collected: %s" % problems)
        expect((sorted(os.listdir(rehearsed)) if os.path.isdir(rehearsed) else []) == sorted(collected_assets(version)),
               "a rehearsal collects every file a release does")
        if os.path.isfile(os.path.join(rehearsed, names["windows"])):
            with open(os.path.join(rehearsed, names["windows"]), "rb") as f:
                expect(f.read() == b"MZ the branch's installer", "a rehearsal's installer is windows-packages-x64's, under the release's name")
        expect(all(any(str(a).endswith("check-source-tarball.py") for a in call) for call in asked),
               "a rehearsal asks git nothing: no tag, no master, no allow-list: %s" % asked)
        for what, changes, needle in (("a tag's pipeline", {"CI_COMMIT_TAG": "v6.3.5"}, "never rehearsed"),
                                      ("master", {"CI_COMMIT_BRANCH": "master"}, "protected batch* branch alone"),
                                      ("another branch", {"CI_COMMIT_BRANCH": "feature-x"}, "protected batch* branch alone"),
                                      ("an unprotected batch branch", {"CI_COMMIT_REF_PROTECTED": "false"}, "is not protected"),
                                      ("no commit", {"CI_COMMIT_SHA": ""}, "not a commit")):
            found = collect(root, None, os.path.join(work, "refused-rehearsal"), dict(rehearsal_env, **changes), run=no_git, api=None, rehearse=True)
            expect(any(needle in p for p in found), "a rehearsal in %s is refused: %s" % (what, found))
            expect(not os.path.isdir(os.path.join(work, "refused-rehearsal")), "a refused rehearsal copies nothing")
        put_back = swap(REHEARSAL_INSTALLER_DIR + "/SHA256SUMS", ("%s  %s\n" % ("0" * 64, branch_installer)).encode("ascii"))
        found = collect(root, None, os.path.join(work, "refused-rehearsal"), rehearsal_env, run=no_git, api=None, rehearse=True)
        expect(any("the file is not the one its build job made" in p for p in found), "a rehearsal's installer unlike its sums is refused: %s" % found)
        put_back()
        expect(signing_identity(rehearsal_env, rehearse=True) == "https://gitlab.com/Progressiverobot/hmailserver//.gitlab-ci.yml@refs/heads/batch42",
               "a rehearsal signs as its branch, an identity no server trusts")
        expect(signing_identity({"CI_PROJECT_PATH": "hmailserver/hmailserver", "CI_COMMIT_TAG": "v6.3.5"})
               == "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v6.3.5", "a release signs as its tag, at either path")
        for what, identity_env in (("another path", {"CI_PROJECT_PATH": "someone/hmailserver", "CI_COMMIT_TAG": "v6.3.5"}),
                                   ("a branch taken as a release", {"CI_PROJECT_PATH": "Progressiverobot/hmailserver", "CI_COMMIT_BRANCH": "batch42"})):
            try:
                signing_identity(identity_env)
                failures.append("a signing identity for %s is refused" % what)
            except ReleaseError:
                pass
        for command in ("publish", "gather", None):
            saved_argv, saved_out = sys.argv, sys.stdout
            sys.argv = ["release-chain.py"] + ([command] if command else []) + ["--rehearse"]
            sys.stdout = captured = io.StringIO()
            try:
                answer = main()
            finally:
                sys.argv, sys.stdout = saved_argv, saved_out
            expect(answer == 2 and "a rehearsal collects, describes and checks; it never" in captured.getvalue(),
                   "--rehearse with %s is refused before anything runs: %s" % (command, captured.getvalue()))

        # release-sbom: the installer's SBOM is made from the signed bytes, never the unsigned.
        signed_dir = os.path.join(work, "release-authenticode")
        os.makedirs(signed_dir)
        shutil.copyfile(os.path.join(out, names["windows"]), os.path.join(signed_dir, names["windows"]))
        try:
            make_installer_sbom(root, tag, env, out, os.path.join(signed_dir, names["windows"]), os.path.join(work, "none.json"),
                                os.path.join(work, "release-sbom"))
            failures.append("an installer Authenticode left unchanged gets no SBOM")
        except ReleaseError as e:
            expect("signed nothing" in str(e), "an installer Authenticode left unchanged gets no SBOM, saying why: %s" % e)
        with open(os.path.join(signed_dir, names["windows"]), "ab") as f:
            f.write(b"a signature")
        try:
            make_installer_sbom(root, tag, env, out, os.path.join(signed_dir, names["windows"]), os.path.join(work, "none.json"),
                                os.path.join(work, "release-sbom"))
            failures.append("an installer SBOM without the component list is refused")
        except ReleaseError as e:
            expect("could not be made" in str(e), "an installer SBOM without the component list is refused, saying why: %s" % e)
        expect(not os.path.isdir(os.path.join(work, "release-sbom")), "and nothing is written")

        # Publishing, against a registry and a releases API held in memory. The
        # installer's SBOM, as release-sbom makes it, describes the installer.
        signed = os.path.join(work, "release-signed")
        shutil.copytree(out, signed)
        with open(os.path.join(signed, sbom_name(names["windows"])), "wb") as f:
            f.write(sbom_of(os.path.join(signed, names["windows"]), "windows-x64"))
        for name in expected_assets(version):
            with open(os.path.join(signed, name + ".cosign.bundle"), "w") as f:
                f.write('{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json", "for": "%s"}' % name)
        # Each primary file's .sigstore.json, byte for byte its bundle, as release-sign.sh copies it.
        for name in primary_assets(version):
            shutil.copyfile(os.path.join(signed, name + ".cosign.bundle"), os.path.join(signed, name + ".sigstore.json"))
        tag_identity = SIGNER_IDENTITY_PREFIX + tag

        def fake_provenance(directory, identity=tag_identity, commit=built, statement_changes=None, bundle_changes=None,
                            certificate_identity=None, key=b""):
            """A bundle as cosign attest-blob --statement writes it: a DSSE envelope of the
            statement, a certificate naming the identity, and a dsse log entry."""
            digests = dict((n, sha256_of(os.path.join(directory, n))) for n in expected_assets(version)
                           if os.path.isfile(os.path.join(directory, n)))
            statement = provenance_statement(version, identity, digests, dict(publish_env, CI_COMMIT_SHA=commit))
            if statement_changes:
                statement_changes(statement)
            bundle = {"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json",
                      "verificationMaterial": {"certificate": {"rawBytes": base64.b64encode(
                          sigstore_certificate.test_certificate(PROJECT_ID, key=key, uris=[certificate_identity or identity])
                      ).decode("ascii")},
                          "tlogEntries": [{"kindVersion": {"kind": "dsse", "version": "0.0.1"}, "integratedTime": "1790000000"}]},
                      "dsseEnvelope": {"payloadType": IN_TOTO_PAYLOAD, "payload": base64.b64encode(json.dumps(statement).encode("utf-8")).decode("ascii"),
                                       "signatures": [{"sig": "MEUC"}]}}
            if bundle_changes:
                bundle_changes(bundle)
            with open(os.path.join(directory, provenance_name(version)), "w", encoding="utf-8") as f:
                json.dump(bundle, f)

        publish_env = {"CI_PROJECT_ID": PROJECT_ID, "CI_API_V4_URL": API_BASE + "/", "CI_PROJECT_PATH": "Progressiverobot/hmailserver",
                       "CI_COMMIT_SHA": built, "CI_PIPELINE_URL": "https://gitlab.com/Progressiverobot/hmailserver/-/pipelines/1"}
        fake_provenance(signed)
        verification = "## Verification\n\nSigned by %s%s." % (SIGNER_IDENTITY_PREFIX, tag)
        api = FakeGitLab()
        payload = None
        try:
            payload = publish(signed, tag, publish_env, api, section, verification)
        except ReleaseError as e:
            failures.append("a complete signed release is published: %s" % e)
        if payload:
            links = payload["assets"]["links"]
            expect(len(links) == 146 and all(l["name"] == urllib.parse.unquote(feed.REGISTRY_LINK.match(l["url"]).group(3)) for l in links),
                   "146 links, each named after the file it points at")
            expect(len(set(l["direct_asset_path"] for l in links)) == 146 and link_problems(links) == [],
                   "every permanent download path is its own, and one GitLab takes: %s" % link_problems(links))
            paths = dict((l["name"], l["direct_asset_path"]) for l in links)
            expect(paths.get(names["deb-arm64"]) == "/downloads/hmailserver_arm64.deb"
                   and paths.get(names["rpm-aarch64"]) == "/downloads/hmailserver.aarch64.rpm",
                   "the AArch64 packages' permanent names are the feed's download names: %s" % paths)
            expect(payload["name"] == "hMailServer 6.3.5" and "The notes." in payload["description"] and SIGNER_IDENTITY_PREFIX in payload["description"],
                   "the notes are the CHANGELOG section and the verification")
            uploads = [c for c in api.calls if c[0] == "PUT"]
            created = [i for i, c in enumerate(api.calls) if c[0] == "POST"]
            expect(len(uploads) == 146 and created and all(api.calls.index(u) < created[0] for u in uploads),
                   "every file is uploaded before the release links to it")
            # And the feed, rendered from what was published, gives each kind of server its file.
            tree, latest, warnings = feed.render(*feed.load(api.feed_get))
            expect("releases/tags/v6.3.5" in tree and latest["tag_name"] == "v6.3.5" and not warnings,
                   "the feed publishes the release as the stable one: %s" % warnings)
            if "releases/tags/v6.3.5" in tree:
                document = json.loads(tree["releases/tags/v6.3.5"].decode("utf-8"))
                for platform in ("deb-arm64", "rpm-aarch64", "appimage-x86_64", "appimage-aarch64"):
                    offered = feed.read_release(document, platform)
                    expect(offered and offered["installer_name"] == names[platform] and offered["bundle_url"],
                           "a server installed from the %s finds its file and its bundle in the feed: %s" % (platform, offered))
                rules = feed.parse_redirects(tree["_redirects"])[0]
                expect(("/download/hmailserver_arm64.deb", registry_url(version, names["deb-arm64"]), 302) in rules
                       and ("/download/hmailserver.aarch64.rpm.cosign.bundle", registry_url(version, names["rpm-aarch64"] + ".cosign.bundle"), 302) in rules,
                       "the site's download names for the AArch64 packages lead to this release's files: %s" % rules)
                expect(("/download/hMailServer-x86_64.AppImage", registry_url(version, names["appimage-x86_64"]), 302) in rules
                       and ("/download/hMailServer-aarch64.AppImage.cosign.bundle", registry_url(version, names["appimage-aarch64"] + ".cosign.bundle"), 302) in rules
                       and len(rules) == 15, "and the AppImages', fifteen rules in all: %s" % rules)
            # A release whose arm64 .deb has lost its bundle's link is one no arm64 Debian
            # server could install: the feed's check after publishing says so.
            for platform in ("deb-arm64", "rpm-aarch64", "appimage-aarch64"):
                unbundled = json.loads(json.dumps(api.releases))
                unbundled[0]["assets"]["links"] = [l for l in unbundled[0]["assets"]["links"]
                                                   if l["name"] != names[platform] + ".cosign.bundle"]
                held_releases, api.releases = api.releases, unbundled
                try:
                    check_feed_takes(api, tag, version)
                    failures.append("a release whose %s has no bundle is refused by the feed's check" % platform)
                except ReleaseError as e:
                    expect("a %s server would find no file with its bundle" % platform in str(e),
                           "a release whose %s has no bundle is refused, saying why: %s" % (platform, e))
                finally:
                    api.releases = held_releases
            expect("milestones" not in payload, "a release is created without a milestone when the project has none of its name")
            expect([p.get("milestones") for p in api.posted] == [["6.3.5"], None],
                   "the release is asked for in the milestone 6.3.5, and when GitLab refuses it, created without one: %s"
                   % [p.get("milestones") for p in api.posted])

        # The milestone of the release's own name, and only that one, is linked, named
        # in the request: the job's token cannot read the Milestones API, which the
        # fake answers 401 as GitLab does, and nothing asks it.
        def milestones_linked(milestones, api_class=FakeGitLab):
            fake = api_class()
            fake.milestones = milestones
            try:
                linked = publish(signed, tag, publish_env, fake, section, verification, render_check=False).get("milestones")
            except ReleaseError as e:
                linked = "refused: %s" % e
            expect(not any(c[1].endswith("/milestones") for c in fake.calls), "the Milestones API, which a job's token cannot use, is not asked")
            return linked, len(fake.posted)

        expect(milestones_linked(["6.3.4", "6.3.5", "6.3.50"]) == (["6.3.5"], 1), "the release is created in the milestone 6.3.5")
        expect(milestones_linked(["6.3.50", "v6.3.5"]) == (None, 2), "a milestone whose title only resembles the version is not linked")

        class Refusing(FakeGitLab):
            """Answers a release POST that names a milestone with status and message."""
            answer = (422, "Validation failed: Milestone releases should belong to the same project as the release")

            def request(self, method, url, body=None, content_type=None):
                if method == "POST" and url.endswith("/releases") and "milestones" in json.loads(body.decode("utf-8")):
                    self.calls.append((method, url))
                    self.posted.append(json.loads(body.decode("utf-8")))
                    return self.answer[0], json.dumps({"message": self.answer[1]}).encode("utf-8"), {}
                return FakeGitLab.request(self, method, url, body, content_type)

        expect(milestones_linked(["6.3.5"], Refusing) == (None, 2), "a milestone GitLab refuses with a 422 still publishes, without one")

        class RefusingOther(Refusing):
            answer = (422, "Validation failed: Tag name is invalid")

        linked, posts = milestones_linked(["6.3.5"], RefusingOther)
        expect(str(linked).startswith("refused: creating the release v6.3.5 answered HTTP 422") and posts == 1,
               "a release GitLab refuses for another reason is not created again without the milestone: %s, %d POST(s)" % (linked, posts))

        # A retry after a run that stopped part-way: what is up already, with the
        # same bytes, is not sent again, and the release is made.
        retried = FakeGitLab()
        up_already = sorted(os.listdir(signed))[:5]
        for name in up_already:
            with open(os.path.join(signed, name), "rb") as f:
                retried.seed(name, f.read())
        try:
            publish(signed, tag, publish_env, retried, section, verification, render_check=False)
            sent = [urllib.parse.unquote(c[1].rsplit("/", 1)[1]) for c in retried.calls if c[0] == "PUT"]
            expect(len(sent) == 141 and not set(sent) & set(up_already), "a retry sends only what is not up already: %s" % sent)
            expect(all(len(v) == 1 for v in [[e for e in retried.files[(PACKAGE, version)] if e["file_name"] == n] for n in os.listdir(signed)]),
                   "a retry leaves one record of each file")
            expect(len(retried.releases) == 1, "a retry creates the release")
        except ReleaseError as e:
            failures.append("a retry after a publish that stopped part-way is published: %s" % e)

        # What publish refuses, having uploaded nothing.
        def refuses(what, needle, assets_dir=signed, env_changes=None, api_object=None):
            e = dict(publish_env)
            e.update(env_changes or {})
            fake = api_object or FakeGitLab()
            before = len(fake.calls)
            try:
                publish(assets_dir, tag, e, fake, section, verification, render_check=False)
                failures.append("%s is refused" % what)
            except ReleaseError as error:
                expect(needle in str(error), "%s is refused, saying why: %s" % (what, error))
            expect(not [c for c in fake.calls[before:] if c[0] in ("PUT", "POST")] or what.startswith("a registry"),
                   "%s uploads and creates nothing" % what)

        refuses("another project", "not %s" % PROJECT_ID, env_changes={"CI_PROJECT_ID": "1"})
        refuses("another GitLab", "which every release link names", env_changes={"CI_API_V4_URL": "https://gitlab.example.com/api/v4"})
        refuses("a release that exists already", "exists already", api_object=api)
        refuses("a file name the registry holds with other bytes", "already holds %s with the SHA-256" % names["rpm-x86_64"],
                api_object=FakeGitLab().seed(names["rpm-x86_64"], b"someone else's package"))
        with open(os.path.join(signed, names["windows"] + ".cosign.bundle"), "rb") as f:
            twice = FakeGitLab().seed(names["windows"] + ".cosign.bundle", f.read()).seed(names["windows"] + ".cosign.bundle", b"{}")
        refuses("a name held twice, once with these bytes and once with others", "already holds %s.cosign.bundle" % names["windows"],
                api_object=twice)
        mismatched = os.path.join(work, "mismatched")
        shutil.copytree(signed, mismatched)
        with open(os.path.join(mismatched, sbom_name(names["windows"])), "wb") as f:
            f.write(sbom_of(os.path.join(mismatched, names["deb-amd64"]), "windows-x64"))
        refuses("an installer SBOM that describes another file", "%s does not describe %s" % (sbom_name(names["windows"]), names["windows"]),
                assets_dir=mismatched)
        unsigned = os.path.join(work, "unsigned")
        shutil.copytree(signed, unsigned)
        os.remove(os.path.join(unsigned, names["windows"] + ".cosign.bundle"))
        refuses("a file without its bundle", "missing hMailServer-6.3.5-x64.exe.cosign.bundle", assets_dir=unsigned)
        extra = os.path.join(work, "extra")
        shutil.copytree(signed, extra)
        with open(os.path.join(extra, "notes.txt"), "w") as f:
            f.write("x")
        refuses("a file that is not a release's", "extra notes.txt", assets_dir=extra)

        class Tampering(FakeGitLab):
            def request(self, method, url, body=None, content_type=None):
                if method == "PUT" and "x64.exe?" in url:
                    body = body + b"!"
                return FakeGitLab.request(self, method, url, body, content_type)

        refuses("a registry that stores other bytes", "not %s" % sha256_of(os.path.join(signed, names["windows"])), api_object=Tampering())

        class Racing(FakeGitLab):
            """Someone uploads another file under a release's name while the job
            uploads: just before the job's own upload of it, or just after."""
            def __init__(self, before):
                FakeGitLab.__init__(self)
                self.before = before

            def request(self, method, url, body=None, content_type=None):
                racing = method == "PUT" and "amd64.deb?" in url
                if racing and self.before:
                    self.seed(names["deb-amd64"], b"slipped in")
                answer = FakeGitLab.request(self, method, url, body, content_type)
                if racing and not self.before:
                    self.seed(names["deb-amd64"], b"slipped in")
                return answer

        refuses("a registry that gains another file of a release's name after the job's upload of it",
                "the registry's %s has the SHA-256" % names["deb-amd64"], api_object=Racing(False))
        refuses("a registry that gains another file of a release's name before the job's upload of it, older than the job's",
                "the registry's %s has the SHA-256" % names["deb-amd64"], api_object=Racing(True))

        # The provenance's statement: every file of the release by its SHA-256, the
        # tag's pipeline as the builder, the commit as the source.
        files = dict((n, os.path.join(signed, n)) for n in expected_assets(version))
        statement_dir = os.path.join(work, "statement")
        os.makedirs(statement_dir)
        written = write_statement(version, tag_identity, files, dict(publish_env, CI_PIPELINE_CREATED_AT="2026-10-02T08:00:00Z"), statement_dir)
        with open(written, encoding="utf-8") as f:
            statement = json.load(f)
        expect(statement["_type"] == IN_TOTO_STATEMENT and statement["predicateType"] == SLSA_PROVENANCE
               and [s_["name"] for s_ in statement["subject"]] == sorted(expected_assets(version))
               and all(s_["digest"]["sha256"] == sha256_of(files[s_["name"]]) for s_ in statement["subject"])
               and statement["predicate"]["runDetails"]["builder"]["id"] == tag_identity
               and statement["predicate"]["buildDefinition"]["resolvedDependencies"][0]["digest"]["gitCommit"] == built
               and statement["predicate"]["runDetails"]["metadata"]["startedOn"] == "2026-10-02T08:00:00Z",
               "the statement names all 57 files by SHA-256, the tag's pipeline as the builder and the commit: %s" % statement.get("subject", [])[:2])
        try:
            write_statement(version, tag_identity, dict(list(files.items())[1:]), publish_env, statement_dir)
            failures.append("a statement that leaves a file out is refused")
        except ReleaseError:
            pass
        expect(provenance_problems(signed, version, tag_identity, built) == [] and copy_problems(signed, version) == [],
               "the signed release's provenance names every file and its copies are its bundles: %s" % (
                   provenance_problems(signed, version, tag_identity, built) + copy_problems(signed, version)))

        def left_out(statement_):
            statement_["subject"] = statement_["subject"][1:]

        def extra(statement_):
            statement_["subject"].append({"name": "notes.txt", "digest": {"sha256": "0" * 64}})

        def other_digest(statement_):
            statement_["subject"][0]["digest"]["sha256"] = "0" * 64

        def other_type(statement_):
            statement_["predicateType"] = "https://slsa.dev/provenance/v0.2"

        def other_log(bundle_):
            bundle_["verificationMaterial"]["tlogEntries"][0]["kindVersion"]["kind"] = "hashedrekord"

        for what, kwargs, needle in (("a file left out", {"statement_changes": left_out}, "does not name every file"),
                                     ("a file that is not the release's", {"statement_changes": extra}, "notes.txt"),
                                     ("a file under another digest", {"statement_changes": other_digest}, "does not name every file"),
                                     ("another predicate", {"statement_changes": other_type}, "SLSA v1 predicate"),
                                     ("another builder", {"identity": SIGNER_IDENTITY_PREFIX + "v6.3.4"}, "names the builder"),
                                     ("another commit", {"commit": "d" * 40}, "as the source it was built from"),
                                     ("a blob signature's log entry", {"bundle_changes": other_log}, "not a dsse entry")):
            twisted = os.path.join(work, "provenance-" + what.replace(" ", "-").replace("'", ""))
            shutil.copytree(signed, twisted)
            fake_provenance(twisted, **kwargs)
            found = provenance_problems(twisted, version, tag_identity, built)
            expect(any(needle in p for p in found), "a provenance with %s is refused: %s" % (what, found))
            if what == "another builder":
                refuses("a release whose provenance another pipeline signed", "names the builder", assets_dir=twisted)
        found = provenance_problems(signed, version, tag_identity.replace("Progressiverobot", "someone"), built)
        expect(any("certificate does not name" in p for p in found), "a provenance whose certificate names another identity is refused: %s" % found)
        # The identity's bytes in the certificate's public key, its SAN another pipeline's:
        # the SAN is what names it.
        planted = os.path.join(work, "provenance-planted")
        shutil.copytree(signed, planted)
        fake_provenance(planted, certificate_identity=tag_identity.replace("Progressiverobot", "someone"),
                        key=tag_identity.encode("utf-8"))
        found = provenance_problems(planted, version, tag_identity, built)
        expect(any("certificate does not name" in p for p in found),
               "a provenance whose certificate names another identity, with this one in its key, is refused: %s" % found)
        fake_provenance(planted, bundle_changes=lambda bundle_: bundle_["verificationMaterial"]["certificate"].update(
            rawBytes=bundle_["verificationMaterial"]["certificate"]["rawBytes"][:-8]))
        found = provenance_problems(planted, version, tag_identity, built)
        expect(any("certificate does not name" in p for p in found), "a provenance whose certificate is cut short is refused: %s" % found)
        uncopied = os.path.join(work, "uncopied")
        shutil.copytree(signed, uncopied)
        os.remove(os.path.join(uncopied, names["deb-arm64"] + ".sigstore.json"))
        refuses("a release without a bundle's .sigstore.json copy", "missing %s.sigstore.json" % names["deb-arm64"], assets_dir=uncopied)
        with open(os.path.join(uncopied, names["deb-arm64"] + ".sigstore.json"), "w") as f:
            f.write("{}")
        refuses("a .sigstore.json that is not its bundle byte for byte", "is not %s.cosign.bundle byte for byte" % names["deb-arm64"], assets_dir=uncopied)
        unproven = os.path.join(work, "unproven")
        shutil.copytree(signed, unproven)
        os.remove(os.path.join(unproven, provenance_name(version)))
        refuses("a release without its provenance", "missing %s" % provenance_name(version), assets_dir=unproven)
        globals()["link_safe"] = lambda name: name
        try:
            refuses("a release with a link GitLab would refuse, found before any upload", "which GitLab refuses")
        finally:
            globals()["link_safe"] = held_safe

        # What the site takes: counted for this release alone (a rehearsal), then beside
        # as many releases of its shape as the project will have made in some years.
        said = []
        digests = dict((n, sha256_of(os.path.join(signed, n))) for n in release_files(version))
        sizes = dict((n, os.path.getsize(os.path.join(signed, n))) for n in release_files(version))
        expect(check_budget(None, tag, version, digests, sizes, out=said.append) == [], "one release is well within every limit")
        expect(len(said) == 2 and "57 files and 146 links (31 primary files, 26 SBOMs, 57 bundles, 31 .sigstore.json copies" in said[0]
               and "_redirects 31 rules" in said[1] and "the feed's 15, the repositories' 16, 16 of them this release's" in said[1],
               "the counts: 146 links; _redirects the feed's fifteen rules (the download page and seven kinds of file with their "
               "bundles) and the repositories' sixteen: %s" % said)
        crowded = FakeGitLab()
        for minor in range(40):
            old = "6.2.%d" % minor
            old_names = release_files(old)
            for n in old_names:
                crowded.files.setdefault((PACKAGE, old), []).append({"id": minor * 1000 + len(crowded.files.get((PACKAGE, old), [])),
                                                                     "file_name": n, "size": 1000, "file_sha256": "a" * 64})
            crowded.releases.append({"tag_name": "v" + old, "name": "hMailServer " + old, "upcoming_release": False,
                                     "released_at": "2026-01-01T00:00:00.000Z", "_links": {"self": feed.PROJECT_URL + "/-/releases/v" + old},
                                     "assets": {"links": release_links(old, old_names)}})
        found = check_budget(crowded, tag, version, digests, sizes, out=lambda line: None)
        expect(any("Pages reads 1000 rules and 65536 bytes" in p and "--keep" in p for p in found),
               "forty releases of 6.3.6's shape are more rules than Pages reads, and the release is refused before it is signed: %s" % found)
        # The answer the refusal names is one the count takes: the repositories kept to
        # the newest ten stable releases, as package-repos.yml's --keep would keep them.
        found = check_budget(crowded, tag, version, digests, sizes, out=lambda line: None, keep=10)
        expect(found == [], "with --keep 10 the same forty-one releases fit, and the release is not refused: %s" % found)
        repos_dir = os.path.join(work, "repos-keep")
        for text, keep in (("    - python3 build/generate-package-repos.py --site public --cache .package-cache\n", 0),
                           ("    - python3 build/generate-package-repos.py --self-test --keep 3\n"
                            "    - python3 build/generate-package-repos.py --site public --keep 12 --cache .package-cache\n", 12),
                           ("    - python3 build/generate-package-repos.py --site public --keep=7\n", 7),
                           ("    # python3 build/generate-package-repos.py --site public --keep 2\n"
                            "    - python3 build/generate-package-repos.py --site public\n", 0)):
            shutil.rmtree(repos_dir, ignore_errors=True)
            os.makedirs(os.path.join(repos_dir, os.path.dirname(REPOS_JOB_FILE)))
            with open(os.path.join(repos_dir, REPOS_JOB_FILE), "w", encoding="utf-8") as f:
                f.write("package-repos:\n  script:\n" + text)
            expect(repository_keep(repos_dir) == keep, "the package-repos job's --keep is read as %d: %d" % (keep, repository_keep(repos_dir)))
        expect(repository_keep(os.path.join(work, "no-such-tree")) == 0, "no package-repos job counts every stable release")
        held_max = MAX_FEED_BYTES
        globals()["MAX_FEED_BYTES"] = 40000
        try:
            found = check_budget(None, tag, version, digests, sizes, out=lambda line: None)
            expect(any("releases.json would be" in p and "MAX_FEED_BYTES" in p for p in found),
                   "a feed larger than a server reads is refused: %s" % found)
        finally:
            globals()["MAX_FEED_BYTES"] = held_max

        # gather, the release-packages job: each build's file from its job's artifact,
        # held to that job's sums, copied with the sums and given its SBOM.
        built_root = os.path.join(work, "gather-checkout")

        def build_file(relative, data):
            path = os.path.join(built_root, local(relative))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)

        for build in gathered_builds(version):
            build_file(build["path"], ("built by %s\n" % build["job"]).encode("utf-8"))
            if build["sums"]:
                build_file(build["sums"], ("%s  %s\n" % (hashlib.sha256(("built by %s\n" % build["job"]).encode("utf-8")).hexdigest(),
                                                         build["listed"])).encode("utf-8"))
            if build["record"]:
                build_file(build["record"] + "/dpkg.txt", b"ii  libc6-dev amd64 2.39\n")
        made = []

        def good_maker(build, path):
            made.append(build["name"])
            with open(path + ".cdx.json", "wb") as f:
                f.write(sbom_of(path, build["kind"]))

        packages = os.path.join(work, "release-packages")
        found = gather(built_root, version, packages, built, make_sbom=good_maker, run=lambda a, **k: Done(0))
        expect(found == [] and sorted(made) == sorted(b["name"] for b in gathered_builds(version) if b["sbom"]),
               "release-packages gathers every file, and the SBOM of each but the runtime's sources: %s" % found)
        expected_gathered = sorted([b["name"] for b in gathered_builds(version)] + [sbom_name(b["name"]) for b in gathered_builds(version)
                                                                                   if b["sbom"]] + [JOB_SUMS])
        expect((sorted(os.listdir(packages)) if os.path.isdir(packages) else []) == expected_gathered
               and len(os.listdir(os.path.join(packages, JOB_SUMS))) == 21,
               "the 22 files, the SBOMs of 21 of them (the runtime's sources is source), and the 21 build jobs' sums (the "
               "tarball has none): %s" % (sorted(os.listdir(packages)) if os.path.isdir(packages) else []))
        # The runtime's sources, which every AppImage names as the source of its
        # runtime's libraries: gathered only from its job, held to that job's sums.
        sources_build = [b for b in gathered_builds(version) if b["name"] == runtime_sources_name(version)][0]
        for what, change, needle in (
                ("the runtime's sources missing", lambda: os.remove(os.path.join(built_root, local(sources_build["path"]))),
                 "%s is not in this job's artifacts (%s): appimage-runtime-sources failed or did not run" % (
                     runtime_sources_name(version), sources_build["path"])),
                ("the runtime's sources without their job's sums", lambda: os.remove(os.path.join(built_root, local(sources_build["sums"]))),
                 "%s, the sums of appimage-runtime-sources, is not in this job's artifacts" % sources_build["sums"]),
                ("the runtime's sources missing from their job's sums", lambda: build_file(sources_build["sums"], b""),
                 "does not name %s" % runtime_sources_name(version)),
                ("the runtime's sources unlike their job's sums",
                 lambda: build_file(sources_build["sums"], ("%s  %s\n" % ("0" * 64, sources_build["listed"])).encode("utf-8")),
                 "the file is not the one its build job made")):
            copy_root = built_root + "-" + str(abs(hash(what)))
            shutil.copytree(built_root, copy_root)
            held_root, built_root = built_root, copy_root
            try:
                change()
                found = gather(built_root, version, os.path.join(work, "gathered-" + str(abs(hash(what)))), built, make_sbom=good_maker,
                               run=lambda a, **k: Done(0))
            finally:
                built_root = held_root
            expect(any(needle in p for p in found), "gather refuses %s: %s" % (what, found))
        first = gathered_builds(version)[0]
        for what, change, maker, needle in (
                ("a build that did not run", lambda: os.remove(os.path.join(built_root, local(first["path"]))), good_maker, "%s failed or did not run" % first["job"]),
                ("a file unlike its job's sums", lambda: build_file(first["sums"], ("%s  %s\n" % ("0" * 64, first["listed"])).encode("utf-8")),
                 good_maker, "the file is not the one its build job made"),
                ("a build without its record", lambda: shutil.rmtree(os.path.join(built_root, local(first["record"]))), good_maker, "its SBOM is made from it"),
                ("an SBOM that could not be made", lambda: None, lambda b, p: (_ for _ in ()).throw(ReleaseError("pwsh is missing")), "pwsh is missing"),
                ("an SBOM of another file", lambda: None,
                 lambda b, p: open(p + ".cdx.json", "wb").write(sbom_of(os.path.join(root, sources(version)[names["deb-amd64"]]), "deb")),
                 "does not describe")):
            copy_root = built_root + "-" + str(abs(hash(what)))
            shutil.copytree(built_root, copy_root)
            held_root, built_root = built_root, copy_root
            try:
                change()
                found = gather(built_root, version, os.path.join(work, "gathered-" + str(abs(hash(what)))), built, make_sbom=maker,
                               run=lambda a, **k: Done(0))
            finally:
                built_root = held_root
            expect(any(needle in p for p in found), "gather refuses %s: %s" % (what, found))
        found = gather(built_root, version, os.path.join(work, "gathered-tarball"), built, make_sbom=good_maker,
                       run=lambda a, **k: Done(1, "PROBLEM: the pax header names another commit"))
        expect(any("is not the source tarball of" in p for p in found), "gather refuses a tarball check-source-tarball.py refuses: %s" % found)
        # The job's own SBOM maker: the component list made once, a package's and an
        # AppImage's SBOM by package-sbom.sh from its build's record, the tarball's by
        # asset-sbom.py, each dated by the commit.
        calls = []

        def fake_run(arguments, **kwargs):
            calls.append((list(arguments), dict(kwargs.get("env") or {})))
            if arguments[0] == "pwsh":
                with open(arguments[arguments.index("-OutputFile") + 1], "w") as f:
                    f.write("[]")
            return Done(0)

        maker = default_sbom_maker(built_root, version, env={"CI_COMMIT_TIMESTAMP": "2026-09-28T04:56:27Z"}, run=fake_run)
        for build in gathered_builds(version):
            if build["sbom"]:
                maker(build, os.path.join(packages, build["name"]))
        listed = [c for c in calls if c[0][0] == "pwsh"]
        by_script = [c for c in calls if any(str(a).endswith("package-sbom.sh") for a in c[0])]
        by_python = [c for c in calls if "--kind" in c[0] and c[0][c[0].index("--kind") + 1] == "source"]
        expect(len(listed) == 1 and len(by_script) == 20 and len(by_python) == 1
               and all(c[1].get("HM_NATIVE_DEPENDENCIES") for c in by_script)
               and all(c[1].get("SOURCE_DATE_EPOCH") == "1790571387" for c in by_script + by_python)
               and all(c[0][-1].replace(os.sep, "/").endswith(b["record"])
                       for c, b in zip(by_script, [b for b in gathered_builds(version) if b["record"]])),
               "the component list is made once; each package's and AppImage's SBOM by package-sbom.sh from its record, the "
               "tarball's by asset-sbom.py, dated by the commit: %d, %d, %d" % (len(listed), len(by_script), len(by_python)))
        try:
            default_sbom_maker(built_root, version, env={}, run=lambda a, **k: Done(1, "", "no such file"))(
                gathered_builds(version)[-1], os.path.join(packages, gathered_builds(version)[-1]["name"]))
            failures.append("an SBOM asset-sbom.py could not make is refused")
        except ReleaseError as e:
            expect("could not be made" in str(e), "an SBOM that could not be made is refused, saying why: %s" % e)

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


# ---------------------------------------------------------------- main

def release_sbom(root, env, assets, installer_path, dependencies_path, out, rehearse=False):
    """The sbom command: the installer's own SBOM (make_installer_sbom), then the
    provenance's statement over every file the release carries - collect's, the
    installer as Authenticode signed it, and its SBOM - for release-sign to attest.
    In a rehearsal the installer is collect's and the identity the branch's."""
    tag = None if rehearse else env.get("CI_COMMIT_TAG", "")
    path = make_installer_sbom(root, tag, env, assets, installer_path, dependencies_path, out, rehearse=rehearse)
    version = read_version_h(root) if rehearse else version_of(tag)
    installer = feed.installer_names(version)["windows"]
    files = dict((name, os.path.join(assets, name)) for name in collected_assets(version))
    files[installer] = installer_path
    files[sbom_name(installer)] = path
    return write_statement(version, signing_identity(env, rehearse), files, env, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", nargs="?", choices=["collect", "sbom", "publish", "gather", "check-provenance"])
    parser.add_argument("--rehearse", action="store_true", help="collect, sbom and check-provenance: the release "
                        "rehearsed on a protected batch* branch, published nowhere")
    parser.add_argument("--out", default=None, help="collect: where the files go (release-assets); sbom: where the "
                        "installer's SBOM and the provenance's statement go (release-sbom); gather: where the gathered "
                        "files go (release-packages)")
    parser.add_argument("--dependencies", default=os.path.join("release-tools", DEPENDENCY_LIST),
                        help="sbom: build/native-dependencies.ps1's list, as linux-sbom wrote it")
    parser.add_argument("--installer-dir", default="release-authenticode", help="sbom: where the signed installer is")
    parser.add_argument("--assets", default="release-signed", help="publish, check-provenance: the signed files and "
                        "their bundles")
    parser.add_argument("--verification", default=os.path.join("release-verified", "verification.md"),
                        help="publish: what release-verify wrote, for the notes")
    parser.add_argument("--self-test", action="store_true", help="test this script with no network, git or token")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    tag = os.environ.get("CI_COMMIT_TAG", "")
    if args.rehearse and args.command not in ("collect", "sbom", "check-provenance"):
        print("REFUSED: a rehearsal collects, describes and checks; it never %s" % (args.command or "runs without a command"))
        return 2
    try:
        if args.command == "sbom":
            if args.rehearse:
                problems = rehearsal_problems(os.environ)
                if problems:
                    raise ReleaseError("; ".join(problems))
                version = read_version_h(ROOT)
                installer_path = os.path.join(ROOT, "release-assets", feed.installer_names(version)["windows"])
            else:
                version = version_of(tag)
                installer_path = os.path.join(ROOT, args.installer_dir, feed.installer_names(version)["windows"])
            release_sbom(ROOT, os.environ, os.path.join(ROOT, "release-assets"), installer_path,
                         os.path.join(ROOT, args.dependencies), os.path.join(ROOT, args.out or "release-sbom"), args.rehearse)
            return 0
        if args.command == "collect":
            if args.rehearse:
                # No token and no registry: a rehearsal reads and publishes nothing of GitLab's.
                problems = collect(ROOT, None, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=None, rehearse=True)
            else:
                token = os.environ.get("CI_JOB_TOKEN", "")
                problems = collect(ROOT, tag, os.path.join(ROOT, args.out or "release-assets"), os.environ, api=GitLab(token) if token else None)
            for problem in problems:
                print("REFUSED: " + problem)
            if problems:
                print("Nothing has been signed or published. %d problem(s)." % len(problems))
                return 1
            version = read_version_h(ROOT) if args.rehearse else version_of(tag)
            print("%s is ready to be signed%s: %d files in %s." % (
                "the rehearsal of %s" % version if args.rehearse else tag, " as the branch, and published nowhere" if args.rehearse else "",
                len(collected_assets(version)), args.out or "release-assets"))
            return 0
        if args.command == "gather":
            version = read_version_h(ROOT)
            problems = gather(ROOT, version, os.path.join(ROOT, args.out or GATHERED), os.environ.get("CI_COMMIT_SHA", ""))
            for problem in problems:
                print("REFUSED: " + problem)
            if problems:
                print("%d problem(s): nothing is gathered for the release." % len(problems))
                return 1
            print("gathered %d files for %s, each compiled one and the tarball with its SBOM, in %s." % (
                len(gathered_builds(version)), version, args.out or GATHERED))
            return 0
        if args.command == "check-provenance":
            version = read_version_h(ROOT) if args.rehearse else version_of(tag)
            identity = signing_identity(os.environ, args.rehearse)
            assets = os.path.join(ROOT, args.assets)
            problems = copy_problems(assets, version) + provenance_problems(assets, version, identity, os.environ.get("CI_COMMIT_SHA", ""))
            for problem in problems:
                print("PROBLEM: " + problem)
            if problems:
                return 1
            print("%s names every file of %s by its SHA-256, built by %s from %s; every primary file's .sigstore.json is "
                  "its bundle." % (provenance_name(version), version, identity, os.environ.get("CI_COMMIT_SHA", "")))
            return 0
        if args.command == "publish":
            with open(os.path.join(ROOT, CHANGELOG), encoding="utf-8") as f:
                notes = changelog_section(f.read(), version_of(tag))
            with open(os.path.join(ROOT, args.verification), encoding="utf-8") as f:
                verification = f.read()
            token = os.environ.get("CI_JOB_TOKEN", "")
            if not token:
                raise ReleaseError("there is no CI_JOB_TOKEN: publish runs in the release-publish job and nowhere else")
            publish(os.path.join(ROOT, args.assets), tag, os.environ, GitLab(token), notes, verification)
            return 0
    except (ReleaseError, feed.FeedError) as e:
        print("REFUSED: %s" % e)
        return 1
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
