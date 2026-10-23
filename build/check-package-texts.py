#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every package text the tree ships says that it is Progressive Robot's fork of
hMailServer, not the original, and credits the original.

The original hMailServer, Martin Knafve's, is developed again (5.7.0 on 28 September
2026, 5.7.1 and a 5.7.2 pre-release on 29 September), and a package called
hMailServer whose texts did not say whose it is reads as the original's: in
zypper info, dnf info and software.opensuse.org (the RPM spec, which OBS and Copr
take from the release's tarball), in apt show and on Launchpad (debian/control,
which the PPA's upload takes from the tarball too, and the .deb CPack makes), on
the AUR (the PKGBUILD), in Alpine (the APKBUILD), in nix search and nix flake
metadata (nix/package.nix, flake.nix), in the Snap Store (snap/snapcraft.yaml), on
a registry and Artifact Hub (the Dockerfile's labels, the chart's Chart.yaml, and
the two READMEs Artifact Hub shows, the chart's and ContainerImage.md), on
Chocolatey and winget (their templates; build/check-package-manifests.py holds the
rendered files too), in a desktop's menu (the AppImages' desktop entry, which
build-appimage.sh writes and AppImageLauncher or appimaged installs), and in the
SBOMs of the release files and of the container images (build/asset-sbom.py's
descriptions of their subjects), and in apt policy and dnf repolist (the origin
and label build/generate-package-repos.py gives each apt suite's Release, which
apt policy prints, the description it gives the suite's Release, and the name it
gives each dnf .repo file). This holds, in each:

  - the summary or title says "Progressive Robot's fork of hMailServer" (the
    desktop entry's Comment and its Name, "hMailServer (Progressive Robot's fork)",
    the SBOMs' descriptions, the apt suites' Label and Description and the dnf
    repositories' name too, the apt suites' Origin being Progressive Robot);
  - the long description says it too and names the original's author, Martin
    Knafve;
  - the summaries are within the length their tools hold them to: an RPM Summary
    and a Debian synopsis 79 characters at most (rpmlint's summary-too-long, Debian
    policy's "less than 80"), a line of an RPM or Debian long description 79 at
    most, the snap's title 40 and its summary 78 (snapcraft's limits);
  - the Chocolatey copyright and authors and winget's 'Copyright' and 'Author' name
    Progressive Robot and the fork, and credit Martin Knafve, as the installer's
    licence does
    (hmailserver/installation/License.rtf: "This software is a fork of hMailServer,
    originally created by Martin Knafve, whose copyright in the original work
    remains"), so that Chocolatey's disclaimer, which names a package's authors as
    those it is not affiliated with, does not name Progressive Robot alone;
  - the packages' maintainer is the project's: debian/control's Maintainer and the
    .deb's (CPACK_DEBIAN_PACKAGE_MAINTAINER, CPACK_PACKAGE_CONTACT) are
    Christopher Holloway <chris.holloway@progressiverobot.com>, and no packaging file
    names the address debian/control named until October 2026, from which
    Launchpad made a placeholder person that is not the PPA's owner;
  - the snap's website is the project's home;
  - every listing shows the fork's own icon, and the same one: the Control
    Panel's (hmailserver/source/Tools/ControlPanel/app.ico, the fork's, which the
    original hMailServer never had), whose 256 by 256 frame, a PNG, is kept byte
    for byte as platform/packaging/hmailserver.png, which this holds to the .ico's
    frame so that the two cannot drift apart (build/make-app-icon.ps1, which
    draws the icon, writes both); the snap names that file as its icon,
    and the Chocolatey package's iconUrl, the chart's icon and the image's
    io.artifacthub.package.logo-url label name its raw address on master, the one
    address a release's package, chart and image can all name (the tags of 6.3.7
    and earlier have no such file).

It reads the files with nothing but Python, as the checkers job runs every
build/check-*.py; that job runs --self-test too, which breaks each rule in a copy of
the tree's own files and sees it refused.

  python build/check-package-texts.py [--self-test]
"""
import os
import re
import shutil
import sys
import tempfile
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
FORK = "Progressive Robot's fork of hMailServer"
ORIGINAL = 'Martin Knafve'
MAINTAINER = 'Christopher Holloway <chris.holloway@progressiverobot.com>'
CONTACT = 'chris.holloway@progressiverobot.com'
OLD_ADDRESS = 'christopher.j.holloway@outlook.com'
HOME = 'https://gitlab.com/hmailserver/hmailserver'

PACKAGING = 'hmailserver/source/Server/platform/packaging'
SPEC = PACKAGING + '/rpm/hmailserver.spec'
CONTROL = PACKAGING + '/debian-src/debian/control'
CMAKE = 'hmailserver/source/Server/CMakeLists.txt'
PKGBUILD = PACKAGING + '/PKGBUILD'
APKBUILD = PACKAGING + '/alpine/APKBUILD'
NIX_PACKAGE = 'nix/package.nix'
FLAKE = 'flake.nix'
SNAP = 'snap/snapcraft.yaml'
DOCKERFILE = PACKAGING + '/docker/Dockerfile'
CHART = PACKAGING + '/helm/hmailserver/Chart.yaml'
CHART_README = PACKAGING + '/helm/hmailserver/README.md'
IMAGE_README = 'hmailserver/docs/ContainerImage.md'
NUSPEC = 'hmailserver/installation/chocolatey/hmailserver.nuspec.in'
WINGET_LOCALE = 'hmailserver/installation/winget/ProgressiveRobot.hMailServer.locale.en-US.yaml.in'
APPIMAGE = PACKAGING + '/build-appimage.sh'
ASSET_SBOM = 'build/asset-sbom.py'
PACKAGE_REPOS = 'build/generate-package-repos.py'

FILES = (SPEC, CONTROL, CMAKE, PKGBUILD, APKBUILD, NIX_PACKAGE, FLAKE, SNAP, DOCKERFILE, CHART,
         CHART_README, IMAGE_README, NUSPEC, WINGET_LOCALE, APPIMAGE, ASSET_SBOM, PACKAGE_REPOS)

# The fork's icon: the Control Panel's .ico, and its 256 by 256 frame as the PNG
# every listing shows, by its path in the tree (the snap) or its raw address on
# master (the Chocolatey package, the chart, the image).
APP_ICO = 'hmailserver/source/Tools/ControlPanel/app.ico'
ICON = PACKAGING + '/hmailserver.png'
ICON_URL = 'https://gitlab.com/hmailserver/hmailserver/-/raw/master/' + ICON
BINARIES = (APP_ICO, ICON)
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def read(root, relative):
    with open(os.path.join(root, relative), encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n')


def flat(text):
    return ' '.join(str(text or '').split())


def read_bytes(root, relative):
    with open(os.path.join(root, relative), 'rb') as f:
        return f.read()


def ico_frame(data, size):
    """The bytes of an .ico's frame of size by size pixels, or None: its ICONDIR
    (reserved 0, type 1, a count) and the count's 16-byte entries, each giving the
    width and height (0 for 256), the frame's length and its offset."""
    if len(data) < 6 or data[0:4] != b'\x00\x00\x01\x00':
        return None
    count = int.from_bytes(data[4:6], 'little')
    for i in range(count):
        entry = data[6 + 16 * i:22 + 16 * i]
        if len(entry) < 16:
            return None
        width, height = entry[0] or 256, entry[1] or 256
        length = int.from_bytes(entry[8:12], 'little')
        offset = int.from_bytes(entry[12:16], 'little')
        if (width, height) == (size, size):
            frame = data[offset:offset + length]
            return frame if len(frame) == length else None
    return None


class Checker:
    def __init__(self):
        self.problems = []

    def problem(self, message):
        self.problems.append(message)

    def found(self, where, value):
        if value is None:
            self.problem('%s: not found' % where)
            return False
        return True

    def fork(self, where, value):
        if self.found(where, value) and FORK not in flat(value):
            self.problem('%s does not say %r, so it reads as the original hMailServer\'s: %r'
                         % (where, FORK, flat(value)[:120]))

    def titled(self, where, value):
        """A title: "hMailServer (Progressive Robot's fork)", or one that says the fork."""
        if self.found(where, value) and FORK not in flat(value) \
                and ('Progressive Robot' not in value or 'fork' not in value):
            self.problem('%s is %r: it names Progressive Robot\'s fork, or it reads as the original\'s'
                         % (where, value))

    def original(self, where, value):
        if self.found(where, value) and ORIGINAL not in flat(value):
            self.problem('%s does not name the original\'s author, %s: %r' % (where, ORIGINAL, flat(value)[:120]))

    def at_most(self, where, value, limit):
        if value is not None and len(value) > limit:
            self.problem('%s is %d characters, more than the %d its tools allow: %r' % (where, len(value), limit, value))

    def lines_at_most(self, where, value, limit):
        for line in (value or '').split('\n'):
            if len(line) > limit:
                self.problem('%s has a line of %d characters, more than %d: %r' % (where, len(line), limit, line))

    def equals(self, where, value, wanted):
        if self.found(where, value) and value.strip() != wanted:
            self.problem('%s is %r, not %r' % (where, value.strip(), wanted))


# ---------------------------------------------------------------- the readers

def spec_texts(text):
    """{'summary', 'description', 'cli summary', 'cli description'} of the spec."""
    lines = text.split('\n')
    found = {}
    package = ''
    i = 0
    while i < len(lines):
        line = lines[i]
        match = re.match(r'^%package\s+(\S+)\s*$', line)
        if match:
            package = match.group(1) + ' '
        match = re.match(r'^Summary:\s*(.*?)\s*$', line)
        if match and package + 'summary' not in found:
            found[package + 'summary'] = match.group(1)
        match = re.match(r'^%description(?:\s+(\S+))?\s*$', line)
        if match:
            name = (match.group(1) + ' ') if match.group(1) else ''
            body = []
            i += 1
            while i < len(lines) and not lines[i].startswith('%'):
                body.append(lines[i])
                i += 1
            found[name + 'description'] = '\n'.join(body).strip('\n')
            continue
        i += 1
    return found


def control_paragraphs(text):
    """[{field: value}] of a deb822 file; a field's continuation lines kept, each
    without its one leading space."""
    paragraphs = []
    for chunk in re.split(r'\n\s*\n', text.strip('\n')):
        fields = {}
        name = None
        for line in chunk.split('\n'):
            if line.startswith('#'):
                continue
            if line[:1] in (' ', '\t') and name:
                fields[name] += '\n' + line[1:]
            else:
                name, _, value = line.partition(':')
                name = name.strip()
                fields[name] = value.strip()
        paragraphs.append(fields)
    return paragraphs


def cmake_set(text, name):
    match = re.search(r'^set\(%s\s+"((?:[^"\\]|\\.)*)"\)' % re.escape(name), text, re.M | re.S)
    return match.group(1) if match else None


def shell_value(text, name):
    match = re.search(r'^%s="([^"]*)"' % re.escape(name), text, re.M)
    return match.group(1) if match else None


def yaml_scalar(text, key):
    match = re.search(r'^%s:[ \t]*(.*?)[ \t]*$' % re.escape(key), text, re.M)
    if not match:
        return None
    value = match.group(1)
    if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
        value = value[1:-1]
    return value


def yaml_block(text, key):
    """A top-level key's block scalar (| or >-), its lines unindented."""
    match = re.search(r'^%s:[ \t]*[|>][-+]?[ \t]*\n((?:[ \t]+[^\n]*\n|\n)*)' % re.escape(key), text + '\n', re.M)
    if not match:
        return None
    return '\n'.join(line.strip() for line in match.group(1).split('\n')).strip('\n')


def label(text, name):
    match = re.search(r'%s="([^"]*)"' % re.escape(name), text)
    return match.group(1) if match else None


def opening(text):
    """A Markdown document's text before its first second-level heading."""
    match = re.search(r'^## ', text, re.M)
    return text[:match.start()] if match else text


def desktop_entry(text):
    """{key: value} of the desktop entry build-appimage.sh writes (its DESKTOP here-document),
    or None where the script writes none."""
    match = re.search(r"<<'DESKTOP'\n(.*?)\nDESKTOP\n", text + '\n', re.S)
    if not match:
        return None
    entry = {}
    for line in match.group(1).split('\n'):
        key, sep, value = line.partition('=')
        if sep and not line.startswith('#'):
            entry[key.strip()] = value.strip()
    return entry


def python_string(text, name):
    """A top-level NAME = "..." of a Python file, its \' and \" read as the quotes."""
    match = re.search(r'^%s = "((?:[^"\\]|\\.)*)"$' % re.escape(name), text, re.M)
    return re.sub(r'\\(.)', r'\1', match.group(1)) if match else None


def xml_element(text, name):
    match = re.search(r'<%s>(.*?)</%s>' % (name, name), text, re.S)
    if not match:
        return None
    value = match.group(1)
    cdata = re.match(r'\s*<!\[CDATA\[(.*)\]\]>\s*$', value, re.S)
    return cdata.group(1) if cdata else value


# ---------------------------------------------------------------- the check

def check(root):
    c = Checker()
    for relative in FILES + BINARIES:
        if not os.path.exists(os.path.join(root, relative)):
            c.problem('%s is not there' % relative)
    if c.problems:
        return c.problems

    icon = read_bytes(root, ICON)
    frame = ico_frame(read_bytes(root, APP_ICO), 256)
    if frame is None or not frame.startswith(PNG_SIGNATURE):
        c.problem('%s has no 256 by 256 frame stored as a PNG, which %s is a copy of' % (APP_ICO, ICON))
    elif icon != frame:
        c.problem('%s is not the 256 by 256 frame of %s byte for byte (%d bytes, the frame %d): the listings\' icon '
                  'and the Control Panel\'s have drifted apart (build/make-app-icon.ps1 writes the two together)'
                  % (ICON, APP_ICO, len(icon), len(frame)))
    if not icon.startswith(PNG_SIGNATURE) or icon[12:16] != b'IHDR' \
            or (int.from_bytes(icon[16:20], 'big'), int.from_bytes(icon[20:24], 'big')) != (256, 256):
        c.problem('%s is not a 256 by 256 PNG' % ICON)

    spec = spec_texts(read(root, SPEC))
    for package in ('', 'cli '):
        where = '%s: the %sSummary' % (SPEC, package)
        c.fork(where, spec.get(package + 'summary'))
        c.at_most(where, spec.get(package + 'summary'), 79)
        where = '%s: the %s%%description' % (SPEC, package)
        c.fork(where, spec.get(package + 'description'))
        c.original(where, spec.get(package + 'description'))
        c.lines_at_most(where, spec.get(package + 'description'), 79)

    paragraphs = control_paragraphs(read(root, CONTROL))
    c.equals('%s: the Maintainer' % CONTROL, paragraphs[0].get('Maintainer') if paragraphs else None, MAINTAINER)
    packages = [p for p in paragraphs if 'Package' in p]
    if [p['Package'] for p in packages] != ['hmailserver', 'hmailserver-cli']:
        c.problem('%s builds %s, not hmailserver and hmailserver-cli' % (CONTROL, [p['Package'] for p in packages]))
    for p in packages:
        description = p.get('Description')
        if not c.found('%s: %s\'s Description' % (CONTROL, p['Package']), description):
            continue
        synopsis, _, extended = description.partition('\n')
        where = '%s: %s\'s synopsis' % (CONTROL, p['Package'])
        c.fork(where, synopsis)
        c.at_most(where, synopsis, 79)
        where = '%s: %s\'s long description' % (CONTROL, p['Package'])
        extended = '\n'.join('' if line == '.' else line for line in extended.split('\n'))
        c.fork(where, extended)
        c.original(where, extended)
        c.lines_at_most(where, extended, 79)

    cmake = read(root, CMAKE)
    summary = cmake_set(cmake, 'CPACK_PACKAGE_DESCRIPTION_SUMMARY')
    c.fork('%s: CPACK_PACKAGE_DESCRIPTION_SUMMARY' % CMAKE, summary)
    c.at_most('%s: CPACK_PACKAGE_DESCRIPTION_SUMMARY' % CMAKE, summary, 79)
    description = cmake_set(cmake, 'CPACK_PACKAGE_DESCRIPTION')
    c.fork('%s: CPACK_PACKAGE_DESCRIPTION' % CMAKE, description)
    c.original('%s: CPACK_PACKAGE_DESCRIPTION' % CMAKE, description)
    c.lines_at_most('%s: CPACK_PACKAGE_DESCRIPTION' % CMAKE, description, 79)
    c.equals('%s: CPACK_RPM_PACKAGE_DESCRIPTION' % CMAKE, cmake_set(cmake, 'CPACK_RPM_PACKAGE_DESCRIPTION'),
             '${CPACK_PACKAGE_DESCRIPTION}')
    c.equals('%s: CPACK_PACKAGE_CONTACT' % CMAKE, cmake_set(cmake, 'CPACK_PACKAGE_CONTACT'), CONTACT)
    c.equals('%s: CPACK_DEBIAN_PACKAGE_MAINTAINER' % CMAKE, cmake_set(cmake, 'CPACK_DEBIAN_PACKAGE_MAINTAINER'),
             'Christopher Holloway <${CPACK_PACKAGE_CONTACT}>')

    for relative in (PKGBUILD, APKBUILD):
        c.fork('%s: pkgdesc' % relative, shell_value(read(root, relative), 'pkgdesc'))

    nix = read(root, NIX_PACKAGE)
    meta = nix[nix.find('meta = {'):] if 'meta = {' in nix else ''
    match = re.search(r'^\s*description = "([^"]*)";', meta, re.M)
    c.fork('%s: meta.description' % NIX_PACKAGE, match.group(1) if match else None)
    match = re.search(r"^\s*longDescription = ''(.*?)'';", meta, re.M | re.S)
    c.fork('%s: meta.longDescription' % NIX_PACKAGE, match.group(1) if match else None)
    c.original('%s: meta.longDescription' % NIX_PACKAGE, match.group(1) if match else None)
    match = re.search(r'^\s*description = "([^"]*)";', read(root, FLAKE), re.M)
    c.fork('%s: description' % FLAKE, match.group(1) if match else None)

    snap = read(root, SNAP)
    c.titled('%s: title' % SNAP, yaml_scalar(snap, 'title'))
    c.at_most('%s: title' % SNAP, yaml_scalar(snap, 'title'), 40)
    c.fork('%s: summary' % SNAP, yaml_scalar(snap, 'summary'))
    c.at_most('%s: summary' % SNAP, yaml_scalar(snap, 'summary'), 78)
    c.fork('%s: description' % SNAP, yaml_block(snap, 'description'))
    c.original('%s: description' % SNAP, yaml_block(snap, 'description'))
    c.equals('%s: website' % SNAP, yaml_scalar(snap, 'website'), HOME)
    c.equals('%s: icon' % SNAP, yaml_scalar(snap, 'icon'), ICON)

    dockerfile = read(root, DOCKERFILE)
    c.titled('%s: org.opencontainers.image.title' % DOCKERFILE, label(dockerfile, 'org.opencontainers.image.title'))
    image_description = label(dockerfile, 'org.opencontainers.image.description')
    c.fork('%s: org.opencontainers.image.description' % DOCKERFILE, image_description)
    c.original('%s: org.opencontainers.image.description' % DOCKERFILE, image_description)
    c.equals('%s: io.artifacthub.package.logo-url' % DOCKERFILE, label(dockerfile, 'io.artifacthub.package.logo-url'),
             ICON_URL)

    chart = read(root, CHART)
    c.equals('%s: icon' % CHART, yaml_scalar(chart, 'icon'), ICON_URL)
    c.fork('%s: description' % CHART, yaml_block(chart, 'description'))
    c.original('%s: description' % CHART, yaml_block(chart, 'description'))
    for relative in (CHART_README, IMAGE_README):
        text = opening(read(root, relative))
        c.fork('%s: the opening, before its first section' % relative, text)
        c.original('%s: the opening, before its first section' % relative, text)

    entry = desktop_entry(read(root, APPIMAGE))
    if c.found('%s: the desktop entry' % APPIMAGE, entry):
        c.titled('%s: the desktop entry\'s Name' % APPIMAGE, entry.get('Name'))
        c.fork('%s: the desktop entry\'s Comment' % APPIMAGE, entry.get('Comment'))
    sbom = read(root, ASSET_SBOM)
    c.fork('%s: ASSET_DESCRIPTION, each release file\'s SBOM description' % ASSET_SBOM,
           python_string(sbom, 'ASSET_DESCRIPTION'))
    c.fork('%s: IMAGE_DESCRIPTION, each container image\'s SBOM description' % ASSET_SBOM,
           python_string(sbom, 'IMAGE_DESCRIPTION'))

    repos = read(root, PACKAGE_REPOS)
    origin = python_string(repos, 'REPO_ORIGIN')
    if c.found('%s: REPO_ORIGIN, the apt suites\' Origin' % PACKAGE_REPOS, origin) \
            and not origin.startswith('Progressive Robot'):
        c.problem('%s: REPO_ORIGIN, the apt suites\' Origin, is %r: the repositories are Progressive Robot\'s'
                  % (PACKAGE_REPOS, origin))
    c.titled('%s: REPO_LABEL, the apt suites\' Label' % PACKAGE_REPOS, python_string(repos, 'REPO_LABEL'))
    c.fork('%s: REPO_DESCRIPTION, the apt suites\' Description' % PACKAGE_REPOS,
           python_string(repos, 'REPO_DESCRIPTION'))
    name = re.search(r'^name=(.*)$', repos, re.M)
    c.titled('%s: REPO_FILE\'s name, each dnf repository\'s' % PACKAGE_REPOS, name.group(1) if name else None)
    for field in ('Origin', 'Label'):
        if re.search(r'%s: (?!%%s)' % field, repos) or re.search(r'\("%s", (?!REPO_)' % field, repos):
            c.problem('%s: a Release names its %s other than by REPO_%s' % (PACKAGE_REPOS, field, field.upper()))

    nuspec = read(root, NUSPEC)
    c.equals('%s: <iconUrl>' % NUSPEC, xml_element(nuspec, 'iconUrl'), ICON_URL)
    for name in ('copyright', 'authors'):
        c.titled('%s: <%s>' % (NUSPEC, name), xml_element(nuspec, name))
        c.original('%s: <%s>' % (NUSPEC, name), xml_element(nuspec, name))
    locale = read(root, WINGET_LOCALE)
    for name in ('Copyright', 'Author'):
        c.titled('%s: %s' % (WINGET_LOCALE, name), yaml_scalar(locale, name))
        c.original('%s: %s' % (WINGET_LOCALE, name), yaml_scalar(locale, name))
    publisher = yaml_scalar(locale, 'Publisher')
    if c.found('%s: Publisher' % WINGET_LOCALE, publisher) and not publisher.startswith('Progressive Robot'):
        c.problem('%s: Publisher is %r: the package\'s publisher, and the installer\'s, is Progressive Robot'
                  % (WINGET_LOCALE, publisher))

    for directory, _, names in os.walk(os.path.join(root, PACKAGING)):
        for name in names:
            path = os.path.join(directory, name)
            try:
                with open(path, encoding='utf-8') as f:
                    text = f.read()
            except (UnicodeDecodeError, OSError):
                continue
            if OLD_ADDRESS in text:
                c.problem('%s names %s, which no Launchpad account holds: the maintainer is %s'
                          % (os.path.relpath(path, root).replace(os.sep, '/'), OLD_ADDRESS, MAINTAINER))
    if OLD_ADDRESS in cmake:
        c.problem('%s names %s: the maintainer is %s' % (CMAKE, OLD_ADDRESS, MAINTAINER))
    return c.problems


# ---------------------------------------------------------------- the self-test

def self_test():
    failures = []

    def expect(what, condition):
        print('%s  %s' % ('ok  ' if condition else 'FAIL', what))
        if not condition:
            failures.append(what)

    originals = {relative: read(ROOT, relative) for relative in FILES}
    originals.update({relative: read_bytes(ROOT, relative) for relative in BINARIES})

    def world(relative=None, change=None):
        work = tempfile.mkdtemp(prefix='hm-package-texts-')
        try:
            for name, text in originals.items():
                if name == relative:
                    text = change(text)
                    if text is None:
                        continue
                path = os.path.join(work, name)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                if isinstance(text, bytes):
                    with open(path, 'wb') as f:
                        f.write(text)
                    continue
                with open(path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(text)
            return check(work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def replaced(old, new, count=1):
        def change(text):
            if old not in text:
                raise AssertionError('the self-test\'s text is not in the file: %r' % old)
            return text.replace(old, new, count)
        return change

    def sub(pattern, new):
        def change(text):
            changed, n = re.subn(pattern, new, text, count=1, flags=re.M)
            if not n:
                raise AssertionError('the self-test\'s pattern is not in the file: %r' % pattern)
            return changed
        return change

    def reencoded(png):
        """The same pixels, compressed again: a PNG that is not the .ico's bytes."""
        chunks, i = [], 8
        while i < len(png):
            length = int.from_bytes(png[i:i + 4], 'big')
            chunks.append((png[i + 4:i + 8], png[i + 8:i + 8 + length]))
            i += 12 + length
        pixels = zlib.decompress(b''.join(body for kind, body in chunks if kind == b'IDAT'))
        out, idat_done = [png[:8]], False
        for kind, body in chunks:
            if kind == b'IDAT':
                if idat_done:
                    continue
                kind, body, idat_done = b'IDAT', zlib.compress(pixels, 1), True
            out.append(len(body).to_bytes(4, 'big') + kind + body
                       + (zlib.crc32(kind + body) & 0xffffffff).to_bytes(4, 'big'))
        changed = b''.join(out)
        if changed == png:
            raise AssertionError('the self-test\'s re-encoded icon is the same bytes')
        return changed

    def frame_changed(ico):
        offset = int.from_bytes(ico[18:22], 'little')
        return ico[:offset + 40] + bytes([ico[offset + 40] ^ 0xff]) + ico[offset + 41:]

    expect('the tree\'s own files: pass', world() == [])
    cases = [
        ('the icon re-encoded, the same pixels in other bytes', ICON, reencoded, 'is not the 256 by 256 frame'),
        ('the icon the .ico\'s 64 by 64 frame', ICON, lambda png: ico_frame(originals[APP_ICO], 64),
         'is not a 256 by 256 PNG'),
        ('the .ico\'s 256 frame changed and the icon not', APP_ICO, frame_changed, 'is not the 256 by 256 frame'),
        ('no icon', ICON, lambda png: None, 'hmailserver.png is not there'),
        ('the nuspec without an iconUrl', NUSPEC, sub(r'^[ \t]*<iconUrl>.*</iconUrl>\n', ''), '<iconUrl>: not found'),
        ('the nuspec\'s iconUrl at a tag, which has no icon', NUSPEC,
         replaced('<iconUrl>https://gitlab.com/hmailserver/hmailserver/-/raw/master/',
                  '<iconUrl>https://gitlab.com/hmailserver/hmailserver/-/raw/v6.3.7/'), '<iconUrl> is'),
        ('the chart without an icon', CHART, sub(r'^icon: .*\n', ''), 'Chart.yaml: icon: not found'),
        ('the image\'s logo-url another picture', DOCKERFILE,
         replaced('logo-url="' + ICON_URL, 'logo-url="https://gitlab.com/hmailserver/hmailserver/-/raw/master/'
                  'hmailserver/docs/images/webmail-inbox.png'), 'logo-url is'),
        ('the snap without an icon', SNAP, sub(r'^icon: .*\n', ''), 'snapcraft.yaml: icon: not found'),
        ('the snap\'s icon the Control Panel\'s .ico', SNAP, sub(r'^icon: .*$', 'icon: ' + APP_ICO),
         'snapcraft.yaml: icon is'),
        ('the RPM Summary without the fork', SPEC, sub(r'^(Summary:\s*).*$', r'\1Mail server for SMTP, IMAP and POP3'),
         'the Summary does not say'),
        ('an RPM Summary of 80 characters', SPEC, sub(r'^(Summary:\s*).*$', r'\1' + FORK + ': ' + 'x' * 39),
         'the Summary is 80 characters'),
        ('the cli package\'s Summary without the fork', SPEC,
         sub(r"^(%package cli\n(?:.*\n)*?Summary:\s*).*$", r"\1Command-line client for hMailServer's REST API"),
         'the cli Summary does not say'),
        ('the RPM %description without the original', SPEC, replaced('Martin Knafve', 'someone'),
         'the %description does not name'),
        ('the cli %description without the fork', SPEC,
         replaced("hmctl, the command-line client of " + FORK, 'hmctl'), 'the cli %description does not say'),
        ('an RPM %description line of 80 characters', SPEC, sub(r'^(%description\n)', r'\1' + 'x' * 80 + '\n'),
         'has a line of 80'),
        ('the outlook.com Maintainer', CONTROL, replaced(MAINTAINER, 'Christopher Holloway <%s>' % OLD_ADDRESS),
         'the Maintainer is'),
        ('the Debian synopsis without the fork', CONTROL,
         sub(r'^Description: .*$', 'Description: mail server for SMTP, IMAP and POP3'), "hmailserver's synopsis"),
        ('the cli package\'s long description without the original', CONTROL,
         replaced(' the original hMailServer by Martin Knafve, which has no REST API.', ' the original.'),
         "hmailserver-cli's long description does not name"),
        ('a Debian long description line of 80 characters', CONTROL,
         sub(r'^(Description: .*\n)', r'\1 ' + 'x' * 80 + '\n'), 'has a line of 80'),
        ('CPack\'s summary without the fork', CMAKE,
         sub(r'^set\(CPACK_PACKAGE_DESCRIPTION_SUMMARY ".*"\)$',
             'set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "hMailServer, a mail server")'),
         'CPACK_PACKAGE_DESCRIPTION_SUMMARY does not say'),
        ('no CPack description', CMAKE, replaced('set(CPACK_PACKAGE_DESCRIPTION "', 'set(HM_UNUSED "'),
         'CPACK_PACKAGE_DESCRIPTION: not found'),
        ('the .rpm without the description', CMAKE,
         replaced('set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")', ''),
         'CPACK_RPM_PACKAGE_DESCRIPTION: not found'),
        ('the old contact', CMAKE, replaced('set(CPACK_PACKAGE_CONTACT "%s")' % CONTACT,
                                            'set(CPACK_PACKAGE_CONTACT "%s")' % OLD_ADDRESS), 'names %s' % OLD_ADDRESS),
        ('no .deb maintainer', CMAKE, sub(r'^set\(CPACK_DEBIAN_PACKAGE_MAINTAINER .*\)$', ''),
         'CPACK_DEBIAN_PACKAGE_MAINTAINER: not found'),
        ('the PKGBUILD\'s pkgdesc without the fork', PKGBUILD, sub(r'^pkgdesc=".*"$', 'pkgdesc="A mail server"'),
         'PKGBUILD: pkgdesc does not say'),
        ('the PKGBUILD\'s old Maintainer', PKGBUILD,
         sub(r'^# Maintainer: .*$', '# Maintainer: Christopher Holloway <%s>' % OLD_ADDRESS),
         'PKGBUILD names %s' % OLD_ADDRESS),
        ('the APKBUILD\'s pkgdesc without the fork', APKBUILD, sub(r'^pkgdesc=".*"$', 'pkgdesc="Mail server"'),
         'APKBUILD: pkgdesc does not say'),
        ('nix meta.description without the fork', NIX_PACKAGE,
         replaced('description = "%s, a mail server' % FORK, 'description = "A mail server'),
         'meta.description does not say'),
        ('nix longDescription without the original', NIX_PACKAGE, replaced('Martin Knafve', 'someone'),
         'meta.longDescription does not name'),
        ('the flake\'s description without the fork', FLAKE,
         replaced('description = "%s' % FORK, 'description = "hMailServer'), 'flake.nix: description does not say'),
        ('the snap\'s title without the fork', SNAP, sub(r'^title: .*$', 'title: hMailServer'),
         'snapcraft.yaml: title is'),
        ('a snap summary of 79 characters', SNAP, sub(r'^summary: .*$', 'summary: ' + FORK + ' ' + 'x' * 39),
         'summary is 79 characters'),
        ('the snap\'s description without the original', SNAP, replaced('Martin Knafve', 'someone'),
         'description does not name'),
        ('the snap\'s website elsewhere', SNAP, sub(r'^website: .*$', 'website: https://www.progressiverobot.com'),
         'website is'),
        ('the image\'s title alone', DOCKERFILE,
         sub(r'org\.opencontainers\.image\.title="[^"]*"', 'org.opencontainers.image.title="hMailServer"'),
         'image.title is'),
        ('the image\'s description without the original', DOCKERFILE,
         replaced('not the original hMailServer by Martin Knafve:', 'not the original:'),
         'image.description does not name'),
        ('the chart\'s description without the fork', CHART, replaced(FORK + ',', 'hMailServer,'),
         'Chart.yaml: description does not say'),
        ('the chart README\'s opening without the fork', CHART_README,
         replaced('This is %s, not the original.' % FORK, 'This is hMailServer.'), 'README.md: the opening'),
        ('ContainerImage.md\'s opening without the original', IMAGE_README,
         replaced('hMailServer is Martin Knafve\'s', 'hMailServer is someone\'s'), 'ContainerImage.md: the opening'),
        # REUSE-IgnoreStart
        ('the nuspec\'s copyright as it was', NUSPEC,
         sub(r'<copyright>.*</copyright>', '<copyright>Copyright (c) 2026 Progressive Robot Ltd. Based on hMailServer</copyright>'),
         '<copyright> does not name'),
        ('the nuspec\'s authors as they were', NUSPEC,
         sub(r'<authors>.*</authors>', '<authors>Christopher Holloway, Progressive Robot Ltd</authors>'),
         '<authors> does not name'),
        ('winget\'s Author as it was', WINGET_LOCALE, sub(r'^Author: .*$', 'Author: Christopher Holloway'),
         'Author does not name'),
        ('winget\'s Copyright as it was', WINGET_LOCALE,
         sub(r'^Copyright: .*$', 'Copyright: Copyright (c) 2026 Progressive Robot Ltd. Based on hMailServer'),
         'Copyright does not name'),
        ('the nuspec\'s copyright the original\'s alone', NUSPEC,
         sub(r'<copyright>.*</copyright>', '<copyright>Copyright (c) 2026 Martin Knafve</copyright>'),
         '<copyright> is'),
        ('the nuspec\'s authors the original\'s alone', NUSPEC,
         sub(r'<authors>.*</authors>', '<authors>Martin Knafve</authors>'), '<authors> is'),
        ('winget\'s Copyright the original\'s alone', WINGET_LOCALE,
         sub(r'^Copyright: .*$', 'Copyright: Copyright (c) 2026 Martin Knafve'), 'Copyright is'),
        # REUSE-IgnoreEnd
        ('winget\'s Author the original\'s alone', WINGET_LOCALE, sub(r'^Author: .*$', 'Author: Martin Knafve'),
         'Author is'),
        ('winget\'s Publisher the original\'s author', WINGET_LOCALE, sub(r'^Publisher: .*$', 'Publisher: Martin Knafve'),
         'Publisher is'),
        ('the AppImages\' desktop entry\'s Comment as it was', APPIMAGE,
         sub(r'^Comment=.*$', 'Comment=A mail server for SMTP, IMAP and POP3'), "desktop entry's Comment does not say"),
        ('the AppImages\' desktop entry\'s Name alone', APPIMAGE, sub(r'^Name=.*$', 'Name=hMailServer'),
         "desktop entry's Name is"),
        ('the release files\' SBOM description as it was', ASSET_SBOM,
         sub(r'^ASSET_DESCRIPTION = ".*"$', 'ASSET_DESCRIPTION = "hMailServer, a mail server for SMTP, IMAP and POP3"'),
         "each release file's SBOM description does not say"),
        ('the container images\' SBOM description as it was', ASSET_SBOM,
         sub(r'^IMAGE_DESCRIPTION = ".*"$', 'IMAGE_DESCRIPTION = "The hMailServer %s container image for linux/%s, built from %s"'),
         "each container image's SBOM description does not say"),
        ('the apt suites\' Origin as it was', PACKAGE_REPOS,
         sub(r'^REPO_ORIGIN = ".*"$', 'REPO_ORIGIN = "hMailServer"'), "the apt suites' Origin, is"),
        ('the apt suites\' Label as it was', PACKAGE_REPOS,
         sub(r'^REPO_LABEL = ".*"$', 'REPO_LABEL = "hMailServer"'), "the apt suites' Label is"),
        ('the apt suites\' Description as it was', PACKAGE_REPOS,
         sub(r'^REPO_DESCRIPTION = ".*"$', 'REPO_DESCRIPTION = "hMailServer for %s, from %s"'),
         "the apt suites' Description does not say"),
        ('the dnf repositories\' name as it was', PACKAGE_REPOS,
         sub(r'^name=.*$', 'name=hMailServer ({dist}, $basearch)'), "each dnf repository's is"),
        ('a binary-<arch>/Release with its own Label', PACKAGE_REPOS,
         replaced('Label: %s\\n', 'Label: hMailServer\\n'), 'names its Label other than by REPO_LABEL'),
        ('the suite Release with its own Origin', PACKAGE_REPOS,
         replaced('("Origin", REPO_ORIGIN)', '("Origin", "hMailServer")'), 'names its Origin other than by REPO_ORIGIN'),
        ('no spec', SPEC, lambda text: None, 'hmailserver.spec is not there'),
    ]
    for what, relative, change, needle in cases:
        problems = world(relative, change)
        said = [p for p in problems if needle in p]
        expect('%s: refused (%s)' % (what, (said or problems or ['nothing said'])[0][:110]), bool(said))
    print('')
    if failures:
        print('%d self-test case(s) failed' % len(failures))
        return 1
    print('Every self-test case passed.')
    return 0


def main(argv):
    if argv[1:] == ['--self-test']:
        return self_test()
    if argv[1:]:
        print(__doc__.strip().splitlines()[-1].strip())
        return 2
    problems = check(ROOT)
    if problems:
        for problem in problems:
            print('FAIL  %s' % problem)
        print('')
        print('%d problem(s): every package text says %r and credits the original, %s.'
              % (len(problems), FORK, ORIGINAL))
        return 1
    print('OK    the RPM spec, debian/control, CPack, the PKGBUILD, the APKBUILD, the nix package and flake, the')
    print('      snap, the image\'s labels, the chart and its READMEs, the Chocolatey and winget templates, the')
    print('      AppImages\' desktop entry, the release files\' and images\' SBOM descriptions and the apt and dnf')
    print('      repositories\' names say %r,' % FORK)
    print('      crediting the original, %s, where they describe it; the maintainer is %s.' % (ORIGINAL, MAINTAINER))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
