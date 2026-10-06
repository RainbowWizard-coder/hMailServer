#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The Helm chart repository on the update feed's site.

Each release the pipeline makes from 6.3.6 on carries the Helm chart as one of its
files, hmailserver-chart-<version>.tgz, packaged by the tag's helm-chart-package job
(.gitlab/ci/helm-chart.yml, build/ci/helm-chart-package.sh) at the release's version
and signed with Sigstore like every other file. This script makes the repository
Helm reads from those files and adds it to the site GitLab Pages serves at
https://updates.progressiverobot.com, beside the update feed
(build/generate-update-feed.py) and the apt and dnf repositories
(build/generate-package-repos.py), so that

    helm repo add hmailserver https://updates.progressiverobot.com/helm/
    helm install mail hmailserver/hmailserver --set database.host=...

works, and Artifact Hub can list the chart (hmailserver/docs/ContainerImage.md).

What it reads is the feed the pages job has just written: public/releases.json,
every release with each file's URL in the package registry, its size and its
SHA-256, as the registry records them. What it adds:

  helm/index.yaml            the repository's index: one entry per release that
                             carries a chart, pre-releases included (Helm lists a
                             version with a suffix only to --devel), newest first,
                             each with the chart's own Chart.yaml as the packaged
                             chart holds it, the file's URL in the package
                             registry, its SHA-256 and the release's date
  helm/artifacthub-repo.yml  Artifact Hub's metadata for the repository: its
                             repository ID, ARTIFACTHUB_REPOSITORY_ID below, the ID
                             Artifact Hub gave when the repository was added there,
                             which gives the listing the Verified publisher label;
                             without an ID, a file that says so and sets nothing
  helm/index.html            a page for a person who opens the address

The charts are not copied onto the site: the index names each by its URL in the
package registry, which Helm fetches directly, as the release page and the update
feed name it. Each is downloaded once here all the same, held to the size and
SHA-256 releases.json gives for it, and read, so that the index describes the chart
that is there: its name must be hmailserver and its version the release's. The
cache (--cache) keeps each by its SHA-256 between pipelines.

The index is YAML written from the data here, every string double-quoted, and read
back with a YAML reader before it is kept. The charts' Chart.yaml files are read
with PyYAML (Debian's python3-yaml in the job's image).

Usage, from the repository root:
  python3 build/ci/helm-repo.py --site public [--cache DIR]
      add helm/ to the site the pages job wrote
  python3 build/ci/helm-repo.py --check public
      read helm/ as Helm would: the index well formed, one entry per version, every
      URL a file of the package registry with its SHA-256
  python3 build/ci/helm-repo.py --self-test [--example DIR [--chart FILE]]
      the selection, the reading of a chart, the index and the refusals, against
      canned releases and charts made here; no network. With --example, also write
      a repository into DIR whose index names its charts relatively, beside it -
      the tree's chart, or FILE, a chart helm package made - for `helm repo add`
      and `helm pull` from a local `python3 -m http.server`
"""

import argparse
import gzip
import hashlib
import html
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHART_DIR = os.path.join(ROOT, "hmailserver", "source", "Server", "platform", "packaging", "helm", "hmailserver")

PROJECT_ID = "86730559"
REGISTRY = "https://gitlab.com/api/v4/projects/" + PROJECT_ID + "/packages/generic/"
SITE = "https://updates.progressiverobot.com"
REPOSITORY_URL = SITE + "/helm/"
FOLDER = "helm"
CHART_NAME = "hmailserver"
# The release's chart file, as build/ci/helm-chart-package.sh names it: the chart's
# name, "-chart-", and the release's version, which Helm requires to be SemVer 2.
CHART_FILE = re.compile(r"^hmailserver-chart-(?P<version>[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?)\.tgz$")
SERVER_TAG = re.compile(r"^v[0-9][A-Za-z0-9._-]{0,62}$")

# The ID Artifact Hub gave the repository when the maintainers added it to the
# organization hmailserver on 6 October 2026, through Artifact Hub's API (kind Helm
# charts, name hmailserver, URL REPOSITORY_URL), in its own words "the ID of the
# Artifact Hub repository where the packages will be published to". package-repos,
# which runs this script after the pages job, writes it into helm/artifacthub-repo.yml,
# which gives the listing its Verified publisher label; Roadmap3.md, section 9b,
# "Artifact Hub: the Helm chart". It is not the container image's ID, which is
# another repository's
# (hmailserver/source/Server/platform/packaging/docker/artifacthub-repo.yml).
ARTIFACTHUB_REPOSITORY_ID = "d8768ed9-b894-45c8-ba44-f7516bcf4206"
UUID = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")

# The keys of a chart's metadata Helm's index carries (helm.sh/helm/v3/pkg/chart,
# Metadata), beside which the index adds urls, created and digest.
METADATA_KEYS = ["apiVersion", "name", "version", "kubeVersion", "description", "type", "keywords", "home", "sources",
                 "dependencies", "maintainers", "icon", "appVersion", "deprecated", "annotations"]
MAX_CHART_BYTES = 5 * 1024 * 1024


class RepoError(Exception):
    pass


QUIET = []


def say(text=""):
    if not QUIET:
        print(text, flush=True)


def load_yaml():
    try:
        import yaml  # noqa: PLC0415 - only the paths that read YAML need it
    except ImportError:
        raise RepoError("PyYAML is not installed (Debian and Ubuntu: python3-yaml; PyPI: PyYAML); it reads the charts' Chart.yaml")
    return yaml


# ---------------------------------------------------------------- what goes in

def select(releases, registry=REGISTRY):
    """The chart of each release in releases.json that carries one, newest version
    first, and a warning for each chart passed over and why."""
    entries, warnings = [], []
    for release in releases:
        tag = release.get("tag_name") or ""
        if release.get("draft") or not SERVER_TAG.match(tag):
            continue
        version = tag[1:]
        for asset in release.get("assets") or []:
            name = asset.get("name") or ""
            match = CHART_FILE.match(name)
            if not match:
                continue
            if match.group("version") != version:
                warnings.append("%s is left out: it names the version %s, and the release is %s" % (name, match.group("version"), version))
                continue
            url = asset.get("browser_download_url") or ""
            expected = registry + "hmailserver/" + version + "/" + name
            if url != expected:
                warnings.append("%s is left out: its URL %s is not the file %s" % (name, url, expected))
                continue
            digest = asset.get("digest") or ""
            if not re.match(r"^sha256:[0-9a-f]{64}$", digest):
                warnings.append("%s is left out: releases.json gives no SHA-256 for it" % name)
                continue
            entries.append({"name": name, "version": version, "url": url, "sha256": digest[len("sha256:"):],
                            "size": int(asset.get("size") or 0), "created": release.get("published_at") or ""})
    versions = [e["version"] for e in entries]
    twice = sorted(set(v for v in versions if versions.count(v) > 1))
    if twice:
        raise RepoError("a chart version is carried twice: %s" % ", ".join(twice))
    entries.sort(key=lambda e: semver_key(e["version"]), reverse=True)
    return entries, warnings


def semver_key(version):
    """SemVer 2's precedence: the numbers, then a release after its pre-releases,
    whose identifiers compare numerically where numeric."""
    core, _, pre = version.partition("-")
    numbers = tuple(int(p) for p in core.split("."))
    if not pre:
        return numbers + ((1,),)
    parts = []
    for identifier in pre.split("."):
        parts.append((0, int(identifier), "") if identifier.isdigit() else (1, 0, identifier))
    return numbers + ((0,) + tuple(parts),)


# ---------------------------------------------------------------- the charts

def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def download(url, attempts=5):
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "hmailserver-helm-repo"})
            with urllib.request.urlopen(request, timeout=120) as response:
                data = response.read(MAX_CHART_BYTES + 1)
            if len(data) > MAX_CHART_BYTES:
                raise RepoError("%s is larger than %d bytes, which no chart of this project is" % (url, MAX_CHART_BYTES))
            return data
        except (urllib.error.URLError, OSError) as e:
            if attempt == attempts - 1:
                raise RepoError("%s could not be downloaded: %s" % (url, e))
            time.sleep(5 * (attempt + 1))
    raise RepoError("%s could not be downloaded" % url)


def fetch(entry, cache=None, get=download):
    """The chart's bytes, from the cache or the registry, held to the size and
    SHA-256 releases.json gives."""
    path = os.path.join(cache, entry["sha256"]) if cache else None
    if path and os.path.isfile(path):
        with open(path, "rb") as f:
            data = f.read()
        if sha256_bytes(data) == entry["sha256"]:
            return data
    data = get(entry["url"])
    digest = sha256_bytes(data)
    if digest != entry["sha256"] or (entry["size"] and len(data) != entry["size"]):
        raise RepoError("%s is %d bytes with SHA-256 %s; releases.json says %d bytes and %s" % (
            entry["url"], len(data), digest, entry["size"], entry["sha256"]))
    if path:
        os.makedirs(cache, exist_ok=True)
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
    return data


def chart_metadata(data, version, what):
    """The Chart.yaml a packaged chart holds, as a dict of the keys Helm's index
    carries, refusing a file that is not this project's chart at this version."""
    yaml = load_yaml()
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            members = archive.getmembers()
            for member in members:
                name = member.name
                if name.startswith("/") or ".." in name.split("/") or "\\" in name:
                    raise RepoError("%s holds the path %s, which no chart may" % (what, name))
                if not (member.isfile() or member.isdir()):
                    raise RepoError("%s holds %s, which is neither a file nor a directory" % (what, name))
            chart_yaml = [m for m in members if m.name == CHART_NAME + "/Chart.yaml"]
            if len(chart_yaml) != 1:
                raise RepoError("%s holds no %s/Chart.yaml" % (what, CHART_NAME))
            text = archive.extractfile(chart_yaml[0]).read().decode("utf-8")
    except (tarfile.TarError, OSError, EOFError, UnicodeDecodeError) as e:
        raise RepoError("%s is not a chart Helm packaged: %s" % (what, e))
    try:
        meta = yaml.safe_load(text)
    except yaml.YAMLError as e:
        raise RepoError("%s's Chart.yaml is not YAML: %s" % (what, e))
    if not isinstance(meta, dict):
        raise RepoError("%s's Chart.yaml is not a mapping" % what)
    if meta.get("apiVersion") != "v2":
        raise RepoError("%s's Chart.yaml says apiVersion %r, not v2" % (what, meta.get("apiVersion")))
    if meta.get("name") != CHART_NAME:
        raise RepoError("%s is the chart %r, not %s" % (what, meta.get("name"), CHART_NAME))
    if str(meta.get("version")) != version:
        raise RepoError("%s is the chart's version %s, and the release is %s: the package was not stamped" % (what, meta.get("version"), version))
    if str(meta.get("appVersion")) != version:
        raise RepoError("%s installs the application %s, and the release is %s: the package was not stamped" % (what, meta.get("appVersion"), version))
    unknown = sorted(k for k in meta if k not in METADATA_KEYS)
    if unknown:
        raise RepoError("%s's Chart.yaml has keys Helm's metadata does not: %s" % (what, ", ".join(unknown)))
    out = {}
    for key in METADATA_KEYS:
        if key in meta and meta[key] is not None:
            out[key] = meta[key]
    out["version"] = version
    out["appVersion"] = version
    return out


# ---------------------------------------------------------------- the index

PLAIN_KEY = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


def scalar(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, str):
        # A JSON string is a YAML double-quoted scalar: its escapes are YAML's too.
        return json.dumps(value, ensure_ascii=True)
    raise RepoError("a value of the kind %s cannot be written into the index" % type(value).__name__)


def emit(value, indent=0):
    """Block YAML for dicts, lists and scalars, every string double-quoted."""
    pad = " " * indent
    lines = []
    if isinstance(value, dict):
        for key, item in value.items():
            name = key if PLAIN_KEY.match(key) else json.dumps(key)
            if isinstance(item, (dict, list)) and item:
                lines.append("%s%s:" % (pad, name))
                lines.extend(emit(item, indent + 2))
            elif isinstance(item, dict):
                lines.append("%s%s: {}" % (pad, name))
            elif isinstance(item, list):
                lines.append("%s%s: []" % (pad, name))
            else:
                lines.append("%s%s: %s" % (pad, name, scalar(item)))
    elif isinstance(value, list):
        for item in value:
            if isinstance(item, dict) and item:
                inner = emit(item, indent + 2)
                lines.append("%s- %s" % (pad, inner[0][indent + 2:]))
                lines.extend(inner[1:])
            elif isinstance(item, (dict, list)):
                lines.append("%s- %s" % (pad, "{}" if isinstance(item, dict) else "[]"))
            else:
                lines.append("%s- %s" % (pad, scalar(item)))
    else:
        lines.append(pad + scalar(value))
    return lines


def index_document(charts):
    """Helm's index (helm.sh/helm/v3/pkg/repo, IndexFile) for the charts, each a
    (metadata, url, sha256, created) - newest version first."""
    versions = []
    for meta, url, digest, created in charts:
        entry = dict(meta)
        entry["urls"] = [url]
        entry["created"] = created
        entry["digest"] = digest
        versions.append(entry)
    document = {"apiVersion": "v1", "entries": {CHART_NAME: versions} if versions else {}}
    if versions:
        document["generated"] = max(v["created"] for v in versions)
    return document


def index_bytes(document):
    text = "# The Helm repository of hMailServer's releases, written by build/ci/helm-repo.py from the update feed.\n"
    text += "\n".join(emit(document)) + "\n"
    yaml = load_yaml()
    if yaml.safe_load(text) != document:
        raise RepoError("the index written does not read back as the index meant; nothing is kept")
    return text.encode("utf-8")


def artifacthub_bytes(repository_id):
    lines = ["# Artifact Hub's metadata for the Helm repository %s (hmailserver/docs/ContainerImage.md," % REPOSITORY_URL,
             "# \"Artifact Hub\"), written by build/ci/helm-repo.py."]
    if repository_id:
        if not UUID.match(repository_id):
            raise RepoError("ARTIFACTHUB_REPOSITORY_ID is %r, not the UUID Artifact Hub gives a repository" % repository_id)
        lines.append("repositoryID: %s" % repository_id)
    else:
        lines.append("# No repository ID yet: it is written here once the repository has been added on artifacthub.io,")
        lines.append("# and gives the listing its Verified publisher label.")
    return ("\n".join(lines) + "\n").encode("utf-8")


def page_bytes(charts):
    e = html.escape
    items = "\n".join('<li><a href="{0}"><code>{1}</code></a>, {2}</li>'.format(e(url), e(meta["version"]), e(created[:10]))
                      for meta, url, _, created in charts) or "<li>No release carries a chart yet.</li>"
    return """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>hMailServer Helm repository</title>
<style>body{{font:16px/1.5 system-ui,sans-serif;max-width:46rem;margin:2rem auto;padding:0 16px}}code{{word-break:break-all}}</style>
</head>
<body>
<h1>hMailServer Helm repository</h1>
<p>The Helm chart of each hMailServer release, which installs that release's container image.</p>
<pre>helm repo add hmailserver {repository}
helm install mail hmailserver/hmailserver --set database.host=...</pre>
<p>Each chart is a file of its release, signed with Sigstore like the others; <a href="https://gitlab.com/hmailserver/hmailserver/-/blob/master/hmailserver/docs/ContainerImage.md">ContainerImage.md</a> says how to check it.</p>
<ul>
{items}
</ul>
</body>
</html>
""".format(repository=e(REPOSITORY_URL), items=items).encode("utf-8")


def repository_files(charts, repository_id):
    return {
        "index.yaml": index_bytes(index_document(charts)),
        "artifacthub-repo.yml": artifacthub_bytes(repository_id),
        "index.html": page_bytes(charts),
    }


# ---------------------------------------------------------------- the site

def read_releases(site):
    path = os.path.join(site, "releases.json")
    if not os.path.isfile(path):
        raise RepoError("%s is not here: the pages job writes the feed first" % path)
    with open(path, encoding="utf-8") as f:
        releases = json.load(f)
    if not isinstance(releases, list):
        raise RepoError("%s is not a list of releases" % path)
    return releases


def write_folder(folder, files):
    if os.path.isdir(folder):
        if os.listdir(folder) and not os.path.isfile(os.path.join(folder, "index.yaml")):
            raise RepoError("%s is there and is not a Helm repository; nothing was written" % folder)
        shutil.rmtree(folder)
    os.makedirs(folder)
    for name, data in sorted(files.items()):
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)


def build(site, cache, get=download, registry=REGISTRY, repository_id=None):
    releases = read_releases(site)
    entries, warnings = select(releases, registry)
    for warning in warnings:
        say("WARNING: " + warning)
    charts = []
    for entry in entries:
        data = fetch(entry, cache, get)
        charts.append((chart_metadata(data, entry["version"], entry["name"]), entry["url"], entry["sha256"], entry["created"]))
        say("%-40s %s  %s" % (entry["name"], entry["sha256"], entry["created"]))
    files = repository_files(charts, ARTIFACTHUB_REPOSITORY_ID if repository_id is None else repository_id)
    write_folder(os.path.join(site, FOLDER), files)
    return charts


def check(site, registry=REGISTRY, relative=False):
    """What Helm needs of the index: problems as sentences."""
    yaml = load_yaml()
    problems = []
    path = os.path.join(site, FOLDER, "index.yaml")
    if not os.path.isfile(path):
        return ["%s is not here" % path]
    with open(path, encoding="utf-8") as f:
        document = yaml.safe_load(f)
    if not isinstance(document, dict) or document.get("apiVersion") != "v1" or not isinstance(document.get("entries"), dict):
        return ["%s is not a Helm index (apiVersion v1 and entries)" % path]
    for name in document["entries"]:
        if name != CHART_NAME:
            problems.append("the index lists a chart named %r" % name)
    seen = set()
    for entry in document["entries"].get(CHART_NAME) or []:
        version = str(entry.get("version"))
        if version in seen:
            problems.append("the index lists %s twice" % version)
        seen.add(version)
        for key in ("apiVersion", "name", "appVersion", "created"):
            if not entry.get(key):
                problems.append("%s's entry has no %s" % (version, key))
        if not re.match(r"^[0-9a-f]{64}$", str(entry.get("digest") or "")):
            problems.append("%s's entry has no SHA-256 digest" % version)
        urls = entry.get("urls") or []
        wanted = "hmailserver-chart-%s.tgz" % version
        if relative:
            if urls != [wanted]:
                problems.append("%s's URL is %s, not %s beside the index" % (version, urls, wanted))
        elif urls != [registry + "hmailserver/" + version + "/" + wanted]:
            problems.append("%s's URL is %s, not the release's file in the package registry" % (version, urls))
    for name in ("artifacthub-repo.yml", "index.html"):
        if not os.path.isfile(os.path.join(site, FOLDER, name)):
            problems.append("%s/%s is not here" % (FOLDER, name))
    return problems


# ---------------------------------------------------------------- the self-test

def ignored(relative, patterns):
    base = os.path.basename(relative)
    for pattern in patterns:
        if pattern.endswith("/"):
            if ("/" + relative + "/").find("/" + pattern) >= 0:
                return True
        elif re.fullmatch(pattern.replace(".", r"\.").replace("*", ".*"), base):
            return True
    return False


def make_chart(version, name=CHART_NAME, chart_version=None, app_version=None, extra=None, drop_chart_yaml=False):
    """A chart archive as helm package lays one out (<name>/Chart.yaml, ...), made
    from the tree's chart with its version and appVersion stamped, sorted and dated
    0 so that the same arguments make the same bytes."""
    patterns = []
    helmignore = os.path.join(CHART_DIR, ".helmignore")
    if os.path.isfile(helmignore):
        with open(helmignore, encoding="utf-8") as f:
            patterns = [l.strip() for l in f if l.strip() and not l.startswith("#")]
    files = []
    for base, dirs, names in os.walk(CHART_DIR):
        dirs.sort()
        for n in sorted(names):
            relative = os.path.relpath(os.path.join(base, n), CHART_DIR).replace(os.sep, "/")
            if not ignored(relative, patterns):
                files.append(relative)
    buffer = io.BytesIO()
    # gzip's header carries a time of its own, which tarfile's w:gz sets to now.
    with gzip.GzipFile(fileobj=buffer, mode="wb", mtime=0) as compressed, \
            tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for relative in files:
            with open(os.path.join(CHART_DIR, *relative.split("/")), "rb") as f:
                data = f.read()
            if relative == "Chart.yaml":
                if drop_chart_yaml:
                    continue
                text = data.decode("utf-8")
                text = re.sub(r"(?m)^name: .*$", "name: %s" % name, text)
                text = re.sub(r"(?m)^version: .*$", "version: %s" % (chart_version or version), text)
                text = re.sub(r"(?m)^appVersion: .*$", "appVersion: \"%s\"" % (app_version or version), text)
                text = text.replace("hmailserver/hmailserver:latest", "hmailserver/hmailserver:%s" % version)
                data = text.encode("utf-8")
            info = tarfile.TarInfo("%s/%s" % (CHART_NAME, relative))
            info.size, info.mtime, info.mode = len(data), 0, 0o644
            archive.addfile(info, io.BytesIO(data))
        for member_name, data in (extra or []):
            info = tarfile.TarInfo(member_name)
            info.size, info.mtime, info.mode = len(data), 0, 0o644
            archive.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def self_test(example=None, chart_file=None):
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    def refused(what, needle, call):
        try:
            call()
        except RepoError as e:
            if needle not in str(e):
                failures.append("%s was refused, but for another reason: %s" % (what, e))
            return
        failures.append("%s was not refused" % what)

    load_yaml()
    charts = {"9.9.9": make_chart("9.9.9"), "9.9.10-rc.1": make_chart("9.9.10-rc.1"), "9.9.7": make_chart("9.9.7")}
    expect(make_chart("9.9.9") == charts["9.9.9"], "the same chart is made as the same bytes")

    def asset(version, name=None, data=None, url=None, digest=None):
        name = name or "hmailserver-chart-%s.tgz" % version
        data = charts.get(version, b"x") if data is None else data
        return {"name": name, "size": len(data), "digest": digest or "sha256:" + sha256_bytes(data),
                "browser_download_url": url or REGISTRY + "hmailserver/" + version + "/" + name}

    releases = [
        {"tag_name": "v9.9.10-rc.1", "published_at": "2026-10-03T10:00:00Z", "prerelease": True,
         "assets": [asset("9.9.10-rc.1"), {"name": "hmailserver_9.9.10-rc.1_amd64.deb", "size": 1, "digest": "sha256:" + "a" * 64,
                                            "browser_download_url": REGISTRY + "hmailserver/9.9.10-rc.1/hmailserver_9.9.10-rc.1_amd64.deb"}]},
        {"tag_name": "v9.9.9", "published_at": "2026-10-02T10:00:00Z", "prerelease": False, "assets": [asset("9.9.9")]},
        {"tag_name": "v9.9.8", "published_at": "2026-10-01T10:00:00Z", "prerelease": False, "assets": []},
        {"tag_name": "v9.9.7", "published_at": "2026-09-30T10:00:00Z", "prerelease": False, "assets": [asset("9.9.7")]},
        {"tag_name": "v9.9.6", "published_at": "2026-09-29T10:00:00Z", "prerelease": False,
         "assets": [asset("9.9.6", url="https://example.org/hmailserver-chart-9.9.6.tgz")]},
        {"tag_name": "v9.9.5", "published_at": "2026-09-28T10:00:00Z", "prerelease": False,
         "assets": [asset("9.9.5", name="hmailserver-chart-9.9.4.tgz")]},
        {"tag_name": "build-inputs-1", "published_at": "2026-09-27T10:00:00Z", "assets": [asset("1.0.0")]},
    ]

    entries, warnings = select(releases)
    expect([e["version"] for e in entries] == ["9.9.10-rc.1", "9.9.9", "9.9.7"],
           "the charts are the three releases' that carry one, newest version first: %s" % [e["version"] for e in entries])
    expect(any("example.org" in w for w in warnings), "a chart outside the package registry is left out, with a warning")
    expect(any("9.9.4" in w for w in warnings), "a chart named for another release is left out, with a warning")
    expect(not any("build-inputs" in w or "1.0.0" in w for w in warnings), "a release that is not a server's is not read")
    expect(semver_key("9.9.10") > semver_key("9.9.10-rc.1") > semver_key("9.9.10-beta.2") > semver_key("9.9.9"),
           "SemVer's order: a release after its pre-releases, rc after beta")
    expect(semver_key("1.0.0-rc.10") > semver_key("1.0.0-rc.2"), "a numeric identifier compares as a number")
    refused("a version carried twice", "carried twice",
            lambda: select(releases + [{"tag_name": "v9.9.9", "published_at": "2026-10-04T10:00:00Z", "assets": [asset("9.9.9")]}]))

    served = {}
    for e in entries:
        served[e["url"]] = charts[e["version"]]
    work = tempfile.mkdtemp(prefix="helm-repo-self-test-")
    try:
        site = os.path.join(work, "public")
        os.makedirs(site)
        with open(os.path.join(site, "releases.json"), "w", encoding="utf-8") as f:
            json.dump(releases, f)
        QUIET.append(True)
        try:
            built = build(site, os.path.join(work, "cache"), get=lambda url: served[url], repository_id="")
        finally:
            QUIET.pop()
        expect(len(built) == 3, "three charts are in the index")
        problems = check(site)
        expect(not problems, "the index written passes its own check: %s" % problems)
        yaml = load_yaml()
        with open(os.path.join(site, FOLDER, "index.yaml"), encoding="utf-8") as f:
            document = yaml.safe_load(f)
        listed = document["entries"][CHART_NAME]
        expect([v["version"] for v in listed] == ["9.9.10-rc.1", "9.9.9", "9.9.7"], "the index lists the versions newest first")
        first = listed[1]
        expect(first["appVersion"] == "9.9.9" and first["digest"] == sha256_bytes(charts["9.9.9"])
               and first["urls"] == [REGISTRY + "hmailserver/9.9.9/hmailserver-chart-9.9.9.tgz"]
               and first["created"] == "2026-10-02T10:00:00Z", "an entry has the release's version, the file's digest, URL and date")
        expect("registry.gitlab.com/hmailserver/hmailserver:9.9.9" in first["annotations"]["artifacthub.io/images"],
               "the entry's Artifact Hub annotations name the release's image")
        expect(first["description"].startswith("hMailServer") and first["type"] == "application" and first["apiVersion"] == "v2",
               "the entry carries the chart's own metadata")
        expect(document["generated"] == "2026-10-03T10:00:00Z", "generated is the newest chart's date, so the same releases write the same index")
        with open(os.path.join(site, FOLDER, "artifacthub-repo.yml"), encoding="utf-8") as f:
            text = f.read()
        expect("repositoryID" not in text.replace("repository ID", ""), "without an ID the Artifact Hub file sets none")
        expect(yaml.safe_load(artifacthub_bytes("0b5ebe9c-0e33-4f5b-9a2d-9a0fbc8a1c55"))["repositoryID"] == "0b5ebe9c-0e33-4f5b-9a2d-9a0fbc8a1c55",
               "an ID is written as Artifact Hub reads it")
        expect((yaml.safe_load(artifacthub_bytes(ARTIFACTHUB_REPOSITORY_ID)) or {}).get("repositoryID") == ARTIFACTHUB_REPOSITORY_ID != "",
               "ARTIFACTHUB_REPOSITORY_ID is the UUID Artifact Hub gave, and package-repos writes it as Artifact Hub reads it")
        refused("an Artifact Hub ID that is not a UUID", "not the UUID", lambda: artifacthub_bytes("not-an-id"))
        # Written again from the cache alone: nothing is fetched.
        def no_network(url):
            raise RepoError("%s was fetched, though the cache holds it" % url)

        QUIET.append(True)
        try:
            again = build(site, os.path.join(work, "cache"), get=no_network, repository_id="")
        finally:
            QUIET.pop()
        expect(len(again) == 3, "a second run takes every chart from the cache")
        with open(os.path.join(site, FOLDER, "index.yaml"), "rb") as f:
            expect(f.read() == repository_files(again, "")["index.yaml"], "the same releases write the same index")

        # Refusals.
        entry = dict(entries[1])
        refused("a chart whose bytes are not releases.json's", "releases.json says",
                lambda: fetch(dict(entry, sha256="0" * 64), None, get=lambda url: charts["9.9.9"]))
        refused("a chart packaged at another version", "was not stamped",
                lambda: chart_metadata(make_chart("9.9.9", chart_version="0.1.0"), "9.9.9", "x"))
        refused("a chart whose application is another version", "installs the application",
                lambda: chart_metadata(make_chart("9.9.9", app_version="latest"), "9.9.9", "x"))
        refused("another project's chart", "not hmailserver", lambda: chart_metadata(make_chart("9.9.9", name="other"), "9.9.9", "x"))
        refused("an archive without Chart.yaml", "holds no", lambda: chart_metadata(make_chart("9.9.9", drop_chart_yaml=True), "9.9.9", "x"))
        refused("an archive with a path out of it", "which no chart may",
                lambda: chart_metadata(make_chart("9.9.9", extra=[("hmailserver/../../etc/x", b"x")]), "9.9.9", "x"))
        refused("bytes that are not an archive", "not a chart Helm packaged", lambda: chart_metadata(b"not a tarball", "9.9.9", "x"))
        os.remove(os.path.join(site, FOLDER, "index.yaml"))
        with open(os.path.join(site, FOLDER, "other"), "w") as f:
            f.write("x")
        refused("a helm/ that is something else", "is not a Helm repository", lambda: write_folder(os.path.join(site, FOLDER), {}))
        # A broken index is caught by the check.
        shutil.rmtree(os.path.join(site, FOLDER))
        write_folder(os.path.join(site, FOLDER), repository_files(built, ""))
        index_path = os.path.join(site, FOLDER, "index.yaml")
        with open(index_path, encoding="utf-8") as f:
            broken = f.read().replace("/hmailserver/9.9.9/", "/hmailserver/9.9.8/")
        with open(index_path, "w", encoding="utf-8") as f:
            f.write(broken)
        expect(any("9.9.9's URL" in p for p in check(site)), "the check catches a URL that is not the release's file")
        # Every string a chart could carry reads back as it was.
        awkward = {"a": "quote \" backslash \\ colon: hash # newline\nend", "b": ["- dash", "", "yes", "null", "1.0", chr(0xe9) + chr(0x2603)],
                   "c": {"x/y.z": "v"}, "d": [], "e": {}, "f": True, "g": 3, "h": [{"k": "v", "l": ["m"]}]}
        expect(yaml.safe_load("\n".join(emit(awkward))) == awkward, "the writer's YAML reads back as the data")
        # The empty repository, before any release carries a chart.
        expect(yaml.safe_load(index_bytes(index_document([]))) == {"apiVersion": "v1", "entries": {}},
               "a repository with no chart is an empty index")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if example and not failures:
        # A site to try Helm against: DIR/helm/, as the Pages site has it, with the
        # chart beside the index and named relatively. Serve DIR and add
        # http://<address>/helm/.
        folder = os.path.join(example, FOLDER)
        if os.path.isdir(folder) and os.listdir(folder):
            failures.append("--example: %s is there already" % folder)
        else:
            if chart_file:
                with open(chart_file, "rb") as f:
                    data = f.read()
                match = CHART_FILE.match(os.path.basename(chart_file))
                if not match:
                    raise RepoError("--chart %s is not named hmailserver-chart-<version>.tgz" % chart_file)
                version = match.group("version")
            else:
                version, data = "9.9.9", charts["9.9.9"]
            name = "hmailserver-chart-%s.tgz" % version
            example_charts = [(chart_metadata(data, version, name), name, sha256_bytes(data), "2026-10-02T10:00:00Z")]
            files = repository_files(example_charts, "")
            files[name] = data
            write_folder(folder, files)
            problems = check(example, relative=True)
            expect(not problems, "the example repository passes the check: %s" % problems)
            say("example repository written to %s: %s, the chart %s" % (folder, name, version))

    for failure in failures:
        print("FAILED: " + failure)
    print("self-test: %d failure(s)" % len(failures))
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--site", help="add helm/ to this site (the pages job's public/)")
    parser.add_argument("--cache", help="keep the charts here by SHA-256 between runs")
    parser.add_argument("--check", metavar="SITE", help="read SITE/helm/ as Helm would")
    parser.add_argument("--self-test", action="store_true", help="test this script with no network")
    parser.add_argument("--example", metavar="DIR", help="with --self-test: write a repository to try Helm against")
    parser.add_argument("--chart", metavar="FILE", help="with --example: the chart to put in it, as helm package made it")
    args = parser.parse_args()
    try:
        if args.self_test:
            return self_test(args.example, args.chart)
        if args.check:
            problems = check(args.check)
            for problem in problems:
                print("PROBLEM: " + problem)
            print("helm-repo check: %d problem(s)" % len(problems))
            return 1 if problems else 0
        if args.site:
            charts = build(args.site, args.cache)
            say("%s/%s: %d chart(s)%s, for %s" % (args.site, FOLDER, len(charts),
                                                  "" if charts else " - no release carries one yet", REPOSITORY_URL))
            return 0
        parser.error("give --site, --check or --self-test")
    except RepoError as e:
        print("helm-repo: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
