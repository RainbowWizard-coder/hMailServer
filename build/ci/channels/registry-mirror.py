#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The registry-mirror job of .gitlab/ci/channels.yml: the release's container image,
copied by its digest with its signature and its SBOM attestation, from the project's
registry to Docker Hub, Quay.io and, when wanted, Amazon ECR Public
(hmailserver/docs/Channels.md, "Container registries").

  1. Dormant: with no destination's credentials it says "not configured" and exits 0.
     A destination is configured by its pair of project CI/CD variables (protected,
     masked): Docker Hub DOCKERHUB_USERNAME and DOCKERHUB_TOKEN (a Read & Write
     access token), Quay.io QUAY_ROBOT_USER and QUAY_ROBOT_TOKEN (a robot account),
     ECR Public AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY with ECR_PUBLIC_REPOSITORY
     (public.ecr.aws/<alias>/hmailserver). Half a pair fails the job.
  2. The release, as every channel takes it (build/ci/channels/release-files.py).
  3. The source, registry.gitlab.com/hmailserver/hmailserver:<version>, resolved to
     its digest, and verified before anything is sent. The image's manifest by that
     digest, and cosign's signature and attestation manifests beside it
     (sha256-<digest>.sig and .att, each tag resolved once and the manifest then
     fetched by its digest), are written as cosign's own OCI layout; then, of those
     very bytes, cosign verify --local-image as this project's pipeline for the tag
     (either path a server trusts), cosign verify-attestation --local-image of its
     CycloneDX SBOM as the same identity, and the project's id in every signing and
     attesting certificate naming that identity, read from the manifests in the layout
     (as for the release's files: required at the group's path, and refused if
     another). No second read of the registry: what cosign verifies is what the id is
     read from.
  4. For each configured destination: regctl image copy of the source BY ITS DIGEST
     to :<version>; and to :<major>.<minor> and :latest only where the source's own tag
     of that name is the same digest, so an older release mirrored late never moves
     them back. Then cosign's signature and attestation manifests (sha256-<digest>.sig
     and .att) are copied there out of the layout: the manifests that were verified,
     not whatever the source's tags answer by then - so neither --digest-tags nor
     --referrers, which would copy the source's as they stand, unverified. Each tag at
     the destination is held to its digest, and the image there is verified again the
     same way, anonymously, from the destination.

The tools are cosign and regctl out of their pinned images (release-files.py). The
credentials reach regctl on its standard input, into a configuration directory of
the job's own that is removed at the end; none is printed or put on a command line.

Usage, from the root of a checkout:
  python3 build/ci/channels/registry-mirror.py [--out DIR]
  python3 build/ci/channels/registry-mirror.py --self-test
"""

import argparse
import base64
import datetime
import hashlib
import hmac
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

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

# The image's one lasting path: build/ci/linux-image.sh pushes nowhere else.
SOURCE = "registry.gitlab.com/hmailserver/hmailserver"
DESTINATIONS = [
    {"name": "Docker Hub", "user": "DOCKERHUB_USERNAME", "secret": "DOCKERHUB_TOKEN",
     "repository": "DOCKERHUB_REPOSITORY", "default": "docker.io/progressiverobot/hmailserver", "host": "docker.io"},
    {"name": "Quay.io", "user": "QUAY_ROBOT_USER", "secret": "QUAY_ROBOT_TOKEN",
     "repository": "QUAY_REPOSITORY", "default": "quay.io/hmailserver/hmailserver", "host": "quay.io"},
    {"name": "Amazon ECR Public", "user": "AWS_ACCESS_KEY_ID", "secret": "AWS_SECRET_ACCESS_KEY",
     "repository": "ECR_PUBLIC_REPOSITORY", "default": None, "host": "public.ecr.aws"},
]
REPOSITORY = re.compile(r"^[a-z0-9.\-]+(:[0-9]+)?(/[a-z0-9]+([._\-][a-z0-9]+)*)+$")
ATTESTATION_TYPE = "cyclonedx"
SIG_CERTIFICATE = "dev.sigstore.cosign/certificate"
PEM_CERTIFICATE = re.compile(r"\A\s*-----BEGIN CERTIFICATE-----\s+([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----\s*\Z")


def say(line=""):
    print(line)
    sys.stdout.flush()


# ---------------------------------------------------------------- the destinations

def configured(environ, fix=None):
    """[(destination, repository)] of every destination whose pair of variables is
    set; Refused on half a pair or a repository on another host."""
    chosen = []
    for d in DESTINATIONS:
        user, secret = environ.get(d["user"]), environ.get(d["secret"])
        if not user and not secret:
            continue
        if not user or not secret:
            raise Refused("%s is set and %s is not: %s takes both (hmailserver/docs/Channels.md)" % (
                d["user"] if user else d["secret"], d["secret"] if user else d["user"], d["name"]))
        repository = (environ.get(d["repository"]) or d["default"] or "").strip()
        if fix and fix.get("registry_destinations", {}).get(d["name"]):
            repository = fix.get("registry_destinations")[d["name"]]
        elif not repository:
            raise Refused("%s needs %s, the repository to push to (public.ecr.aws/<alias>/hmailserver)"
                          % (d["name"], d["repository"]))
        elif not repository.startswith(d["host"] + "/") or not REPOSITORY.match(repository):
            raise Refused("%s is %r: %s's repository is %s/<namespace>/<name>, and its credentials go nowhere else"
                          % (d["repository"], repository, d["name"], d["host"]))
        chosen.append((d, repository))
    return chosen


# ---------------------------------------------------------------- Amazon ECR Public's password (SigV4)

def sigv4_signing_key(secret, date, region, service):
    key = ("AWS4" + secret).encode("utf-8")
    for part in (date, region, service, "aws4_request"):
        key = hmac.new(key, part.encode("utf-8"), hashlib.sha256).digest()
    return key


def sigv4_headers(method, url, headers, body, access_key, secret, region, service, when, session=None):
    """The headers of a request signed with AWS Signature Version 4."""
    parsed = urllib.parse.urlsplit(url)
    amz_date = when.strftime("%Y%m%dT%H%M%SZ")
    date = amz_date[:8]
    signed = dict((k.lower(), v.strip()) for k, v in headers.items())
    signed["host"] = parsed.netloc
    signed["x-amz-date"] = amz_date
    if session:
        signed["x-amz-security-token"] = session
    names = sorted(signed)
    query = "&".join(sorted("%s=%s" % (urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~"))
                            for k, v in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)))
    canonical = "\n".join([method, parsed.path or "/", query, "".join("%s:%s\n" % (n, signed[n]) for n in names),
                           ";".join(names), hashlib.sha256(body).hexdigest()])
    scope = "%s/%s/%s/aws4_request" % (date, region, service)
    to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canonical.encode("utf-8")).hexdigest()])
    signature = hmac.new(sigv4_signing_key(secret, date, region, service), to_sign.encode("utf-8"), hashlib.sha256).hexdigest()
    out = dict((k, v) for k, v in signed.items() if k != "host")
    out["authorization"] = "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s" % (
        access_key, scope, ";".join(names), signature)
    return out


def ecr_public_password(environ, http=None):
    """The password regctl logs in to public.ecr.aws with, as user AWS: ECR Public's
    GetAuthorizationToken (us-east-1, the only region it answers in)."""
    url = "https://api.ecr-public.us-east-1.amazonaws.com/"
    body = b"{}"
    headers = sigv4_headers("POST", url, {"content-type": "application/x-amz-json-1.1",
                                          "x-amz-target": "SpencerFrontendService.GetAuthorizationToken"},
                            body, environ["AWS_ACCESS_KEY_ID"], environ["AWS_SECRET_ACCESS_KEY"], "us-east-1",
                            "ecr-public", datetime.datetime.now(datetime.timezone.utc), environ.get("AWS_SESSION_TOKEN"))
    status, _, answer = (http or rf.http)(url, data=body, headers=headers, method="POST")
    try:
        token = json.loads(answer.decode("utf-8"))["authorizationData"]["authorizationToken"]
        user, _, password = base64.b64decode(token).decode("utf-8").partition(":")
    except (ValueError, KeyError, TypeError):
        raise Refused("ECR Public refused the AWS credentials (HTTP %s)" % status)
    if status != 200 or user != "AWS" or not password:
        raise Refused("ECR Public answered HTTP %s without a token" % status)
    return password


# ---------------------------------------------------------------- the tools

class Tools:
    def __init__(self, cosign, regctl, config, run=subprocess.run, insecure=()):
        self.cosign = cosign
        self.regctl = regctl
        self.run = run
        # regctl keeps its logins in $HOME/.regctl: a home of the job's own.
        self.env = dict(os.environ, HOME=config, DOCKER_CONFIG=os.path.join(config, "docker"))
        self.insecure = tuple(insecure)

    def regctl_(self, *arguments, stdin=None):
        answer = self.run([self.regctl] + list(arguments), capture_output=True, text=True, input=stdin, env=self.env)
        return answer.returncode, (answer.stdout or "").strip(), (answer.stderr or "").strip()

    def regctl_bytes(self, *arguments):
        """regctl's standard output as the bytes it wrote: a manifest's body or a blob,
        whose digest is of exactly those bytes."""
        answer = self.run([self.regctl] + list(arguments), capture_output=True, env=self.env)
        return answer.returncode, answer.stdout or b"", (answer.stderr or b"").decode("utf-8", "replace").strip()

    def cosign_(self, *arguments):
        flags = []
        if any(a.startswith(self.insecure) for a in arguments if self.insecure):
            flags = ["--allow-insecure-registry", "--allow-http-registry"]
        answer = self.run([self.cosign] + list(arguments[:1]) + flags + list(arguments[1:]), capture_output=True,
                          text=True, env=dict(self.env, HOME=os.environ.get("HOME", self.env["HOME"])))
        return answer.returncode, (answer.stdout or "").strip(), (answer.stderr or "").strip()

    def digest(self, reference):
        code, out, err = self.regctl_("image", "digest", reference)
        if code != 0 or not re.match(r"^sha256:[0-9a-f]{64}$", out):
            return None
        return out


def verify_flags(tools):
    """cosign's image signatures here are its tag form, not the new bundle format."""
    code, out, err = tools.cosign_("verify", "--help")
    return ["--new-bundle-format=false"] if "--new-bundle-format" in (out + err) else []


# ---------------------------------------------------------------- what cosign verifies, fetched once

# The fields of an image manifest and of a descriptor as cosign's reader (go-containerregistry's
# v1.Manifest and v1.Descriptor) names them: a signature or attestation manifest with any
# other key is refused.
MANIFEST_FIELDS = ("schemaVersion", "mediaType", "config", "layers", "annotations", "subject", "artifactType")
DESCRIPTOR_FIELDS = ("mediaType", "size", "digest", "data", "urls", "annotations", "platform", "artifactType")
IMAGE_MANIFEST_TYPES = ("application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json")
IMAGE_INDEX_TYPES = ("application/vnd.oci.image.index.v1+json", "application/vnd.docker.distribution.manifest.list.v2+json")
DIGEST = re.compile(r"^sha256:[0-9a-f]{64}$")
# cosign's own OCI layout (cosign save, its pkg/oci/layout), which cosign verify
# --local-image and verify-attestation --local-image read: each part is the first
# manifest of index.json whose "kind" annotation names it.
KIND = "kind"
KIND_IMAGE = "dev.cosignproject.cosign/image"
KIND_INDEX = "dev.cosignproject.cosign/imageIndex"
KIND_SIGNATURES = "dev.cosignproject.cosign/sigs"
KIND_ATTESTATIONS = "dev.cosignproject.cosign/atts"
# cosign's tags beside an image: its signatures and its attestations.
ATTACHED = ((".sig", KIND_SIGNATURES, "signature"), (".att", KIND_ATTESTATIONS, "attestation"))


def json_object_once(pairs):
    """A JSON object's members as a dict; ValueError for a key that is not ASCII, or a
    key there twice in any case: Go's encoding/json, which cosign reads a manifest with,
    matches a field's name whatever its case, and through Unicode's case mappings (a
    long s, U+017F, is an s), and the last of two such keys wins, where Python's json
    takes each key as it is spelled. No key outside ASCII is taken, so no mapping of
    one onto a field's name is left to know."""
    found, folded = {}, set()
    for key, value in pairs:
        if not key.isascii():
            raise ValueError("a key that is not ASCII, %s" % json.dumps(key))
        if key.lower() in folded:
            raise ValueError("the key %s twice, in some case" % json.dumps(key))
        folded.add(key.lower())
        found[key] = value
    return found


def attached_manifest(text):
    """(media type, config, layers) of cosign's signature or attestation manifest
    (sha256-<hex>.sig or .att) as cosign reads it; ValueError for one it could read
    otherwise. cosign reads it with Go's encoding/json into go-containerregistry's
    v1.Manifest, where "Layers", "LAYERS" or "layers" with a long s (U+017F) is
    "layers" too, "Annotations" a layer's "annotations", and the last of two such keys
    wins (cosign 2.6.5 verifies a signature so hidden from a reader that takes only
    "layers"); and a registry that serves the tag as an index hands cosign the child
    its "Manifests" names, beside the "layers" a reader of the body would take (cosign
    verifies that child). So every key of the manifest, and of its config and each
    layer, must be one of v1.Manifest's or v1.Descriptor's fields spelled as the field
    is; no key of any object may be there twice in any case or be other than ASCII; the
    manifest must be an image manifest; and each annotation must be a string, as Go's
    map[string]string takes it."""
    manifest = json.loads(text, object_pairs_hook=json_object_once)
    if not isinstance(manifest, dict):
        raise ValueError("it is not a JSON object")

    def only(item, fields, what):
        for key in item:
            if key not in fields:
                raise ValueError("%s holds the key %s, which is not a field of %s spelled as it is (%s)"
                                 % (what, json.dumps(key), "v1.Manifest" if fields is MANIFEST_FIELDS else "v1.Descriptor",
                                    ", ".join(fields)))
    only(manifest, MANIFEST_FIELDS, "the manifest")
    media = manifest.get("mediaType", IMAGE_MANIFEST_TYPES[0])
    if media not in IMAGE_MANIFEST_TYPES:
        raise ValueError("it is not an image manifest, whose layers are the signatures")
    config, layers = manifest.get("config"), manifest.get("layers")
    if not isinstance(config, dict) or not isinstance(layers, list) or not all(isinstance(layer, dict) for layer in layers):
        raise ValueError("its config and layers are not descriptors")
    for descriptor in [config] + layers:
        only(descriptor, DESCRIPTOR_FIELDS, "a descriptor")
        annotations = descriptor.get("annotations", {})
        if not isinstance(annotations, dict) or not all(isinstance(v, str) for v in annotations.values()):
            raise ValueError("a descriptor's annotations are not an object of strings")
        size = descriptor.get("size")
        if not isinstance(descriptor.get("digest"), str) or not DIGEST.match(descriptor["digest"]) \
                or type(size) is not int or size < 0:
            raise ValueError("a descriptor without a SHA-256 digest and a size")
    return media, config, layers


def sha256_digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def manifest_at(tools, repository, digest):
    """The manifest repository@digest, the bytes the registry sent, which must be digest's."""
    code, body, err = tools.regctl_bytes("manifest", "get", "%s@%s" % (repository, digest), "--format", "raw-body")
    if code != 0:
        raise Refused("%s@%s could not be read: %s" % (repository, digest, err[-300:]))
    if sha256_digest(body) != digest:
        raise Refused("%s@%s answered a manifest whose digest is %s" % (repository, digest, sha256_digest(body)))
    return body


def blob_at(tools, repository, descriptor):
    """The blob a descriptor names, which must be its digest and its size."""
    code, data, err = tools.regctl_bytes("blob", "get", repository, descriptor["digest"])
    if code != 0:
        raise Refused("the blob %s of %s could not be read: %s" % (descriptor["digest"], repository, err[-300:]))
    if sha256_digest(data) != descriptor["digest"] or len(data) != descriptor["size"]:
        raise Refused("%s answered a blob for %s that is not it" % (repository, descriptor["digest"]))
    return data


def fetch_layout(tools, repository, digest, layout):
    """The image repository@digest with its signature and attestation manifests, written
    as cosign's own OCI layout at layout, for cosign verify --local-image and
    verify-attestation --local-image and for the certificate reader alike: each tag
    (sha256-<hex>.sig, .att) resolved once, each manifest then fetched once by its
    digest and held to it, with its config and layers (not the image's layers, which no
    verification reads). Nothing of them is fetched again, by cosign or by the reader,
    so a registry that answers a tag one way and then another cannot have cosign verify
    one signature manifest and the reader read another; and cosign reads each manifest
    as this one says it is, whatever a registry calls it. {kind: digest} of the three."""
    if os.path.exists(layout):
        shutil.rmtree(layout)
    blobs = os.path.join(layout, "blobs", "sha256")
    os.makedirs(blobs)

    def keep(data):
        with open(os.path.join(blobs, sha256_digest(data).split(":", 1)[1]), "wb") as f:
            f.write(data)

    image = manifest_at(tools, repository, digest)
    try:
        media = json.loads(image.decode("utf-8"), object_pairs_hook=json_object_once).get("mediaType")
    except (ValueError, UnicodeDecodeError, AttributeError):
        media = None
    if media not in IMAGE_MANIFEST_TYPES + IMAGE_INDEX_TYPES:
        raise Refused("%s@%s is not an image manifest or an image index naming its media type" % (repository, digest))
    kind = KIND_IMAGE if media in IMAGE_MANIFEST_TYPES else KIND_INDEX
    keep(image)
    manifests = [{"mediaType": media, "size": len(image), "digest": digest, "annotations": {KIND: kind}}]
    held = {kind: digest}
    for suffix, kind, what in ATTACHED:
        tag = "%s:sha256-%s%s" % (repository, digest.split(":", 1)[1], suffix)
        at = tools.digest(tag)
        if not at:
            raise Refused("%s could not be read: the image has no %s there" % (tag, what))
        body = manifest_at(tools, repository, at)
        try:
            media, config, layers = attached_manifest(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as error:
            raise Refused("the image's %s manifest (%s@%s) is not one cosign and this read alike: %s"
                          % (what, repository, at, error))
        for descriptor in [config] + layers:
            keep(blob_at(tools, repository, descriptor))
        keep(body)
        manifests.append({"mediaType": media, "size": len(body), "digest": at, "annotations": {KIND: kind}})
        held[kind] = at
    with open(os.path.join(layout, "oci-layout"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"imageLayoutVersion": "1.0.0"}, f)
    with open(os.path.join(layout, "index.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"schemaVersion": 2, "mediaType": IMAGE_INDEX_TYPES[0], "manifests": manifests}, f, indent=1)
    return held


def layout_manifest(layout, kind):
    """The bytes of the manifest cosign's layout reader takes as kind: the first of
    index.json's manifests annotated so (its imageByAnnotation); None when there is none."""
    with open(os.path.join(layout, "index.json"), encoding="utf-8") as f:
        index = json.load(f, object_pairs_hook=json_object_once)
    for descriptor in index.get("manifests") or []:
        if (descriptor.get("annotations") or {}).get(KIND) == kind and DIGEST.match(descriptor.get("digest") or ""):
            with open(os.path.join(layout, "blobs", "sha256", descriptor["digest"].split(":", 1)[1]), "rb") as f:
                return f.read()
    return None


def certificate_ids(body, identity, what):
    """The repository id (or None) of EVERY certificate in a signature or attestation
    manifest that names identity in its subject alternative name, each read from its
    extensions (build/ci/sigstore-certificate.py). Every one: cosign verify and
    verify-attestation pass when one signature verifies, and which one it was they do
    not say, so a layer added beside it - whose certificate nothing but this ever reads
    - must not be the one read instead. Refused when none names identity, or when a
    certificate cannot be read."""
    try:
        _, _, layers = attached_manifest((body or b"").decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as error:
        raise Refused("the image's %s manifest is not one cosign and this read alike: %s" % (what, error))
    recorded = []
    for layer in layers:
        text = (layer.get("annotations") or {}).get(SIG_CERTIFICATE) or ""
        if not text:
            continue
        pem = PEM_CERTIFICATE.match(text)
        try:
            if not pem:
                raise ValueError("it is not one PEM certificate")
            der = base64.b64decode("".join(pem.group(1).split()), validate=True)
            if identity not in rf.sigstore_certificate.uris(der):
                continue
            recorded.append(rf.repository_id(der))
        except ValueError as error:
            raise Refused("a certificate of the image's %ss could not be read: %s" % (what, error))
    if not recorded:
        raise Refused("no %s of the image carries a certificate naming %s" % (what, identity))
    return recorded


def held_to_the_project(recorded, identity, what):
    """The rule the release's files are held to (release-files.py, verify_file): each
    certificate's repository id this project's where there is one, and one there at
    the group's path."""
    for found in recorded:
        if found is not None and found != rf.PROJECT_ID:
            raise Refused("the image's %s certificate names the repository id %s, not %s" % (what, found, rf.PROJECT_ID))
        if found is None and identity.startswith(rf.GROUP_REPOSITORY + "//"):
            raise Refused("the image's %s certificate, at the group's path, records no repository id" % what)


def verify_image(tools, repository, digest, identities, issuer, layout):
    """(identity, repository id, {kind: digest}) of an image that verifies, by digest,
    as one of the identities, with its SBOM attested by the same: fetched once into
    layout (fetch_layout), verified there by cosign, and every signing and attesting
    certificate naming the identity held to the project's id there; Refused otherwise."""
    reference = "%s@%s" % (repository, digest)
    held = fetch_layout(tools, repository, digest, layout)
    flags = verify_flags(tools)
    identity, said = None, ""
    for candidate in identities:
        code, out, err = tools.cosign_("verify", *flags, "--local-image", "--certificate-identity", candidate,
                                       "--certificate-oidc-issuer", issuer, layout)
        said = err or out
        if code == 0:
            try:
                answers = json.loads(out)
            except ValueError:
                raise Refused("cosign verify answered no JSON for %s" % reference)
            if not answers or any((a.get("critical") or {}).get("image", {}).get("docker-manifest-digest") != digest
                                  for a in answers):
                raise Refused("the signature cosign verified is not over %s" % digest)
            identity = candidate
            break
    if identity is None:
        raise Refused("%s does not verify as signed by %s: nothing was copied.\n  cosign said: %s"
                      % (reference, " or ".join(identities), said.splitlines()[-1] if said else "nothing"))
    code, out, err = tools.cosign_("verify-attestation", *flags, "--local-image", "--type", ATTESTATION_TYPE,
                                   "--certificate-identity", identity, "--certificate-oidc-issuer", issuer, layout)
    if code != 0:
        raise Refused("%s has no %s SBOM attested by %s: nothing was copied.\n  cosign said: %s"
                      % (reference, ATTESTATION_TYPE, identity, (err or out).splitlines()[-1] if (err or out) else ""))
    signed = certificate_ids(layout_manifest(layout, KIND_SIGNATURES), identity, "signature")
    held_to_the_project(signed, identity, "signing")
    # The SBOM's attestation too: cosign verify-attestation holds its certificate to the
    # identity, and nothing else holds it to the project.
    held_to_the_project(certificate_ids(layout_manifest(layout, KIND_ATTESTATIONS), identity, "attestation"), identity,
                        "attestation's")
    return identity, (rf.PROJECT_ID if rf.PROJECT_ID in signed else None), held


# ---------------------------------------------------------------- the job

def mirror_tags(tools, source, digest, version):
    """The tags to push: <version>, and <major>.<minor> and latest where the source's
    own tag of that name is this digest."""
    tags = [version]
    for tag in (version.rsplit(".", 1)[0], "latest"):
        if tools.digest("%s:%s" % (source, tag)) == digest:
            tags.append(tag)
        else:
            say("the source's :%s is not %s: not moved" % (tag, digest))
    return tags


def registry_mirror(out, environ=None, run=subprocess.run, tools_for=None):
    environ = os.environ if environ is None else environ
    fix = rf.fixture(environ)
    destinations = configured(environ, fix)
    if not destinations:
        say("not configured: none of Docker Hub (DOCKERHUB_USERNAME, DOCKERHUB_TOKEN), Quay.io (QUAY_ROBOT_USER,")
        say("QUAY_ROBOT_TOKEN) or Amazon ECR Public (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, ECR_PUBLIC_REPOSITORY)")
        say("is set, so nothing was copied (hmailserver/docs/Channels.md, 'Container registries').")
        return 0

    version, where = rf.release_version(environ, fix)
    say("the release: %s (from %s)" % (version, where))
    say("to: %s" % ", ".join("%s (%s)" % (d["name"], r) for d, r in destinations))
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    config = tempfile.mkdtemp(prefix="hm-registry-mirror-")
    try:
        source = (fix.get("registry_source") if fix else None) or SOURCE
        identities = (fix.get("image_identities") if fix else None) or rf.identities(version)
        issuer = (fix.get("image_issuer") if fix else None) or rf.ISSUER
        insecure = ("localhost", "127.0.0.1") if fix else ()
        if tools_for is not None:
            tools = tools_for(config)
        else:
            tools = Tools(rf.tool_path("cosign", out, environ), rf.tool_path("regctl", out, environ), config, run, insecure)
        for host in insecure:
            for port in ("5000",):
                tools.regctl_("registry", "set", "%s:%s" % (host, port), "--tls", "disabled")

        digest = tools.digest("%s:%s" % (source, version))
        if not digest:
            raise Refused("%s:%s is not in the project's registry: the release's image jobs (container-image-deb and"
                          " those after it) have not pushed it (hmailserver/docs/ContainerImage.md, 'Publishing')"
                          % (source, version))
        say("the source: %s:%s is %s" % (source, version, digest))
        # What was verified stays in the job's folder, cosign's own layout of it: the
        # image's manifest, and its signature and attestation manifests with their blobs.
        verified = os.path.join(out, "verified", "source")
        identity, recorded, held = verify_image(tools, source, digest, identities, issuer, verified)
        say("verified: %s@%s, signed (%s) and its SBOM attested (%s) as %s (repository id: %s)"
            % (source, digest, held[KIND_SIGNATURES], held[KIND_ATTESTATIONS], identity, recorded or "none recorded"))
        tags = mirror_tags(tools, source, digest, version)

        record = {"version": version, "source": source, "digest": digest, "identity": identity,
                  "signature": held[KIND_SIGNATURES], "attestation": held[KIND_ATTESTATIONS], "destinations": {}}
        for number, (destination, repository) in enumerate(destinations):
            host = repository.split("/", 1)[0]
            if destination["name"] == "Amazon ECR Public" and not (fix and fix.get("registry_destinations")):
                user, password = "AWS", ecr_public_password(environ)
            else:
                user, password = environ[destination["user"]], environ[destination["secret"]]
            code, _, err = tools.regctl_("registry", "login", host, "--user", user, "--pass-stdin", stdin=password)
            password = None
            if code != 0:
                raise Refused("%s refused the login of %s: %s" % (destination["name"], user, err[-300:]))
            for tag in tags:
                code, _, err = tools.regctl_("image", "copy", "%s@%s" % (source, digest), "%s:%s" % (repository, tag))
                if code != 0:
                    raise Refused("the copy to %s:%s failed: %s" % (repository, tag, err[-400:]))
            # Then the signature and attestation manifests that were verified, out of the
            # layout they were verified in: not the source's tags read again, which could
            # answer otherwise by now.
            attached = "%s:sha256-%s" % (repository, digest.split(":", 1)[1])
            for suffix, kind, what in ATTACHED:
                code, _, err = tools.regctl_("image", "copy", "ocidir://%s@%s" % (verified, held[kind]), attached + suffix)
                if code != 0:
                    raise Refused("the copy of the verified %s to %s failed: %s" % (what, attached + suffix, err[-400:]))
            for tag in tags:
                at = tools.digest("%s:%s" % (repository, tag))
                if at != digest:
                    raise Refused("%s:%s is %s after the copy, not %s" % (repository, tag, at, digest))
            for suffix, kind, what in ATTACHED:
                at = tools.digest(attached + suffix)
                if at != held[kind]:
                    raise Refused("%s is %s after the copy, not the %s verified, %s" % (attached + suffix, at, what, held[kind]))
            tools.regctl_("registry", "logout", host)
            verify_image(tools, repository, digest, [identity], issuer, os.path.join(out, "verified", "destination-%d" % number))
            say("%s: %s:%s, each %s, verified there as %s"
                % (destination["name"], repository, ",".join(tags), digest, identity))
            record["destinations"][destination["name"]] = {"repository": repository, "tags": tags}
        with open(os.path.join(out, "mirror.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(record, f, indent=1, sort_keys=True)
            f.write("\n")
        say("PUBLISHED: %s of v%s to %s." % (digest, version, ", ".join(d["name"] for d, _ in destinations)))
        return 0
    finally:
        shutil.rmtree(config, ignore_errors=True)


# ---------------------------------------------------------------- self-test

def real_cosign_cases(expect, cosign, work):
    """The layout fetch_layout writes, verified by a real cosign (channels-check's, out
    of the pinned image). With a key of the test's own: cosign verifies the signature and
    the attestation from it, refuses one another key made, and reads a signature
    manifest's keys as Go does - a signature under "Layers" or "layer<long s>", which
    fetch_layout refuses for that reason. Keyless, as the release's pipeline signs, with
    fake-sigstore.py's authority: verify_image end to end, which takes the image this
    project's pipeline signed and attested, and refuses one another project's pipeline
    signed or attested at the identity, which cosign itself verifies."""
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        print("skip  the layouts against a real cosign: the cryptography package is not installed")
        return
    blobs, tags = {}, {}

    def put(data):
        blobs[sha256_digest(data)] = data
        return sha256_digest(data)

    def descriptor(data, media, annotations=None):
        found = {"mediaType": media, "size": len(data), "digest": put(data)}
        if annotations is not None:
            found["annotations"] = annotations
        return found

    key, other = ec.generate_private_key(ec.SECP256R1()), ec.generate_private_key(ec.SECP256R1())
    public = os.path.join(work, "cosign.pub")
    with open(public, "wb") as f:
        f.write(key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    repository = "registry.test/hmailserver/hmailserver"
    image = json.dumps({"schemaVersion": 2, "mediaType": IMAGE_MANIFEST_TYPES[1], "config": descriptor(
        b'{"architecture":"amd64","os":"linux","config":{}}', "application/vnd.docker.container.image.v1+json"),
        "layers": []}).encode()
    digest = put(image)
    config = descriptor(b'{"architecture":"","os":"","config":{},"rootfs":{"type":"layers","diff_ids":[]}}',
                        "application/vnd.oci.image.config.v1+json")

    def signature_layer(signer):
        payload = json.dumps({"critical": {"identity": {"docker-reference": repository},
                                           "image": {"docker-manifest-digest": digest},
                                           "type": "cosign container image signature"}, "optional": None}).encode()
        signature = base64.b64encode(signer.sign(payload, ec.ECDSA(hashes.SHA256()))).decode()
        return descriptor(payload, "application/vnd.dev.cosign.simplesigning.v1+json",
                          {"dev.cosignproject.cosign/signature": signature})

    def attestation_layer(signer):
        statement = json.dumps({"_type": "https://in-toto.io/Statement/v0.1", "predicateType": "https://cyclonedx.org/bom",
                                "subject": [{"name": repository, "digest": {"sha256": digest.split(":", 1)[1]}}],
                                "predicate": {"bomFormat": "CycloneDX", "specVersion": "1.6", "components": []}}).encode()
        kind = b"application/vnd.in-toto+json"
        signed = b"DSSEv1 %d %s %d %s" % (len(kind), kind, len(statement), statement)
        envelope = json.dumps({"payloadType": kind.decode(), "payload": base64.b64encode(statement).decode(), "signatures": [
            {"keyid": "", "sig": base64.b64encode(signer.sign(signed, ec.ECDSA(hashes.SHA256()))).decode()}]}).encode()
        return descriptor(envelope, "application/vnd.dsse.envelope.v1+json", {"dev.cosignproject.cosign/signature": "",
                                                                             "predicateType": "https://cyclonedx.org/bom"})

    def manifest(layers, extra=""):
        return put((json.dumps({"schemaVersion": 2, "mediaType": IMAGE_MANIFEST_TYPES[0], "config": config,
                                "layers": layers})[:-1] + extra + "}").encode())

    class Registry:
        def regctl_bytes(self, *arguments):
            at = arguments[2].split("@", 1)[1] if arguments[:2] == ("manifest", "get") else arguments[3]
            return (0, blobs[at], "") if at in blobs else (1, b"", "not found")

        def digest(self, reference):
            return tags.get(reference)

    attached = "%s:sha256-%s" % (repository, digest.split(":", 1)[1])
    tags[attached + ".sig"] = manifest([signature_layer(key)])
    tags[attached + ".att"] = manifest([attestation_layer(key)])
    layout = os.path.join(work, "cosign-layout")
    fetch_layout(Registry(), repository, digest, layout)

    def verify(kind="verify"):
        command = [cosign, kind, "--key", public, "--insecure-ignore-tlog", "--new-bundle-format=false", "--local-image"]
        if kind == "verify-attestation":
            command[2:2] = ["--type", ATTESTATION_TYPE]
        answer = subprocess.run(command + [layout], capture_output=True, text=True)
        return answer.returncode, answer.stdout, ((answer.stderr or "").strip().splitlines() or [""])[-1][:100]

    code, out, said = verify()
    expect("cosign %s verifies the signature in the layout fetch_layout wrote, over the image's digest (%s)"
           % (os.path.basename(cosign), said), code == 0 and digest in out)
    code, _, said = verify("verify-attestation")
    expect("  ... and the SBOM's attestation (%s)" % said, code == 0)

    def with_signatures(body):
        """The layout with its signature manifest replaced by body (the manifest's
        digest), every blob made here beside it."""
        path = os.path.join(layout, "index.json")
        with open(path, encoding="utf-8") as f:
            index = json.load(f)
        for entry in index["manifests"]:
            if entry["annotations"][KIND] == KIND_SIGNATURES:
                entry["digest"], entry["size"] = body, len(blobs[body])
        for at, data in blobs.items():
            with open(os.path.join(layout, "blobs", "sha256", at.split(":", 1)[1]), "wb") as f:
                f.write(data)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f)

    with_signatures(manifest([signature_layer(other)]))
    code, _, said = verify()
    expect("  ... and refuses a signature another key made (%s)" % said, code != 0)
    for what, extra in (("\"Layers\"", '"Layers"'), ("\"layer\\u017f\"", '"layer\\u017f"')):
        # One that verifies nothing under "layers", the one that does under the other key.
        good = signature_layer(key)
        body = manifest([signature_layer(other)], ", %s: [%s]" % (extra, json.dumps(good)))
        tags[attached + ".sig"] = body
        try:
            fetch_layout(Registry(), repository, digest, os.path.join(work, "refused-layout"))
            fetched = "fetched"
        except Refused as error:
            fetched = str(error)
        with_signatures(body)
        code, _, said = verify()
        expect("  ... and verifies a signature under %s beside \"layers\", as Go reads it (%s), which fetch_layout "
               "refuses (%s)" % (what, said, fetched[-60:]), code == 0 and fetched != "fetched")

    # Keyless, as the release's pipeline signs and attests the image: fake-sigstore.py's
    # authority, handed to cosign by the variables its tag form reads it from, so no
    # network; verify_image end to end, cosign verifying the layout it wrote.
    fake = load("fake_sigstore", os.path.join(HERE, "fake-sigstore.py")).FakeSigstore()
    environment = dict(os.environ, HOME=work, **fake.legacy_environment(work))
    group = rf.identities("9.9.9")[1]

    def keyless(attestation=False, rid="86730559", identity=group):
        if attestation:
            payload = json.dumps({"_type": "https://in-toto.io/Statement/v0.1", "predicateType": "https://cyclonedx.org/bom",
                                  "subject": [{"name": repository, "digest": {"sha256": digest.split(":", 1)[1]}}],
                                  "predicate": {"bomFormat": "CycloneDX", "specVersion": "1.6", "components": []}}).encode()
        else:
            payload = json.dumps({"critical": {"identity": {"docker-reference": repository},
                                               "image": {"docker-manifest-digest": digest},
                                               "type": "cosign container image signature"}, "optional": None}).encode()
        data, annotations = fake.image_layer(payload, identity, attestation=attestation, repository=rf.GROUP_REPOSITORY,
                                             repository_id=rid)
        return descriptor(data, "application/vnd.dsse.envelope.v1+json" if attestation else
                          "application/vnd.dev.cosign.simplesigning.v1+json", annotations)

    class Keyless(Registry):
        def cosign_(self, *arguments):
            answer = subprocess.run([cosign] + list(arguments), capture_output=True, text=True, env=environment)
            return answer.returncode, (answer.stdout or "").strip(), (answer.stderr or "").strip()

    def by_cosign(kind):
        """cosign's own verdict on the layout verify_image fetched, as the identity."""
        command = [cosign, kind, "--new-bundle-format=false", "--local-image", "--certificate-identity", group,
                   "--certificate-oidc-issuer", rf.ISSUER] + (["--type", ATTESTATION_TYPE] if kind != "verify" else [])
        return subprocess.run(command + [os.path.join(work, "keyless")], capture_output=True, text=True,
                              env=environment).returncode == 0

    for what, signature, attestation, needle in (
            ("this project's pipeline signed and attested it", keyless(), keyless(True), None),
            ("another project's pipeline signed it at the identity", keyless(rid="1267388815"), keyless(True),
             "signing certificate names the repository id 1267388815"),
            ("another project's pipeline attested its SBOM at the identity", keyless(), keyless(True, rid="1267388815"),
             "attestation's certificate names the repository id 1267388815"),
            ("  ... with no id, at the group's path", keyless(), keyless(True, rid=None),
             "attestation's certificate, at the group's path, records no repository id"),
            ("another tag's pipeline signed and attested it", keyless(identity=group.replace("v9.9.9", "v9.9.8")),
             keyless(True, identity=group.replace("v9.9.9", "v9.9.8")), "does not verify as signed")):
        tags[attached + ".sig"] = manifest([signature])
        tags[attached + ".att"] = manifest([attestation])
        try:
            said = verify_image(Keyless(), repository, digest, rf.identities("9.9.9"), rf.ISSUER, os.path.join(work, "keyless"))
            said = "verified as %s, repository id %s" % (said[0], said[1])
        except Refused as error:
            said = str(error)
        verdicts = (by_cosign("verify"), by_cosign("verify-attestation"))
        if needle is None:
            expect("keyless: %s - cosign %s verifies it from the layout, and the job takes it (%s)"
                   % (what, os.path.basename(cosign), said[:90]), said == "verified as %s, repository id 86730559" % group
                   and verdicts == (True, True))
        else:
            expect("keyless: %s - refused (%s), where cosign verify and verify-attestation say %s"
                   % (what, said.splitlines()[0][:90], verdicts), needle in said
                   and (verdicts == (True, True) or needle == "does not verify as signed"))


def self_test(cosign=None):
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

    # SigV4, against AWS's own published vectors: the signing key of the IAM
    # documentation's example, and the test suite's get-vanilla request.
    key = sigv4_signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20150830", "us-east-1", "iam")
    expect("SigV4's signing key is AWS's documented one",
           key.hex() == "c4afb1cc5771d871763a393e44b703571b55cc28424d1a5e86da6ed3c154a4b9")
    signed = sigv4_headers("GET", "https://example.amazonaws.com/", {}, b"", "AKIDEXAMPLE",
                           "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "us-east-1", "service",
                           datetime.datetime(2015, 8, 30, 12, 36, 0, tzinfo=datetime.timezone.utc))
    expect("SigV4 signs the test suite's get-vanilla as AWS does", signed["authorization"].endswith(
        "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"))

    # The destinations.
    expect("no variable: no destination", configured({}) == [])
    chosen = configured({"DOCKERHUB_USERNAME": "u", "DOCKERHUB_TOKEN": "t", "QUAY_ROBOT_USER": "hmailserver+ci",
                         "QUAY_ROBOT_TOKEN": "q"})
    expect("Docker Hub and Quay by their pairs, to the documented repositories",
           [r for _, r in chosen] == ["docker.io/progressiverobot/hmailserver", "quay.io/hmailserver/hmailserver"])
    refused("half a pair", lambda: configured({"DOCKERHUB_TOKEN": "t"}), "DOCKERHUB_TOKEN is set and DOCKERHUB_USERNAME")
    refused("a Docker Hub repository on another host", lambda: configured({
        "DOCKERHUB_USERNAME": "u", "DOCKERHUB_TOKEN": "t", "DOCKERHUB_REPOSITORY": "evil.example/x/y"}), "go nowhere else")
    refused("ECR Public without its repository", lambda: configured({"AWS_ACCESS_KEY_ID": "a", "AWS_SECRET_ACCESS_KEY": "s"}),
            "needs ECR_PUBLIC_REPOSITORY")
    said = []
    held_say = globals()["say"]
    globals()["say"] = said.append
    try:
        expect("with no destination: 'not configured', exit 0, nothing read",
               registry_mirror("unused", {}) == 0 and said[0].startswith("not configured"))
    finally:
        globals()["say"] = held_say

    # The job against a registry, regctl and cosign in memory.
    version = "9.9.9"
    group = rf.identities(version)[1]
    work = tempfile.mkdtemp(prefix="hm-mirror-test-")
    blobs = {}

    def put(data):
        blobs[sha256_digest(data)] = data
        return sha256_digest(data)

    def descriptor(data, media, annotations=None):
        found = {"mediaType": media, "size": len(data), "digest": put(data)}
        if annotations is not None:
            found["annotations"] = annotations
        return found

    config_of = descriptor(b'{"architecture":"amd64","os":"linux","config":{}}', "application/vnd.docker.container.image.v1+json")
    image = json.dumps({"schemaVersion": 2, "mediaType": IMAGE_MANIFEST_TYPES[1], "config": config_of, "layers": []}).encode()
    digest = put(image)
    other_image = put(image.replace(b'"layers": []', b'"layers": [], "annotations": {"another": "image"}'))

    def layer(rid="86730559", key=b"", uris=None, good=True, attestation=False):
        """A signature layer (or an attestation's) whose certificate carries the
        repository id rid, key as its public key's bytes and uris (the identity when
        None) in its SAN; with good, the in-memory cosign finds its signature good for
        an identity its SAN names, and with good an identity, for that one whatever
        the SAN's URIs are (as a name of another kind in it would be)."""
        text = base64.b64encode(rf.test_certificate(rid, key=key, uris=uris or [group])).decode()
        pem = "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n" % "\n".join(
            text[i:i + 64] for i in range(0, len(text), 64))
        payload = json.dumps({"critical": {"image": {"docker-manifest-digest": digest}}, "rid": rid,
                              "key": base64.b64encode(key).decode(), "attestation": attestation}).encode()
        return descriptor(payload, "application/vnd.dsse.envelope.v1+json" if attestation else
                          "application/vnd.dev.cosign.simplesigning.v1+json",
                          {"dev.cosignproject.cosign/signature": "" if attestation else "MEUC", SIG_CERTIFICATE: pem,
                           "test/verifies": good if isinstance(good, str) else "yes" if good else "no"})

    signature_config = descriptor(b'{"architecture":"","os":"","config":{},"rootfs":{"type":"layers","diff_ids":[]}}',
                                  "application/vnd.oci.image.config.v1+json")

    def manifest(layers, extra="", key="layers"):
        """cosign's signature or attestation manifest of layers under key, with the JSON
        members extra after them."""
        return put((json.dumps({"schemaVersion": 2, "mediaType": IMAGE_MANIFEST_TYPES[0], "config": signature_config,
                                key: layers})[:-1] + extra + "}").encode())

    def go_fold(value):
        """value as Go's encoding/json reads it into cosign's structs: a key in any case,
        or with a long s, is the field's, and the last of two wins."""
        if isinstance(value, dict):
            found = {}
            for key, item in value.items():
                found[key.replace("\u017f", "s").lower()] = go_fold(item)
            return found
        if isinstance(value, list):
            return [go_fold(v) for v in value]
        return value

    class FakeTools:
        """regctl and cosign against blobs, the source's tags (each a digest, or a list
        answered in turn, the last one again and again) and the destinations'."""

        def __init__(self, signed=True, attested=True, sig=None, att=None, moved=("9.9", "latest"), copies_ok=True,
                     attached_ok=True, lying=()):
            self.calls, self.moved, self.copies_ok, self.attached_ok, self.lying = [], moved, copies_ok, attached_ok, lying
            hexdigest = digest.split(":", 1)[1]
            self.tags = {"%s:%s" % (SOURCE, version): [digest],
                         "%s:sha256-%s.sig" % (SOURCE, hexdigest): sig or [manifest([layer(good=signed)])],
                         "%s:sha256-%s.att" % (SOURCE, hexdigest): att or [manifest([layer(good=attested, attestation=True)])]}
            for tag in ("9.9", "latest"):
                self.tags["%s:%s" % (SOURCE, tag)] = [digest if tag in moved else other_image]
            self.tags = dict((k, v if isinstance(v, list) else [v]) for k, v in self.tags.items())

        def resolve(self, reference):
            answers = self.tags.get(reference)
            if not answers:
                return None
            return answers.pop(0) if len(answers) > 1 else answers[0]

        def regctl_(self, *arguments, stdin=None):
            self.calls.append(("regctl",) + arguments + (("<stdin>",) if stdin else ()))
            if arguments[:2] == ("image", "copy"):
                source, target = arguments[-2], arguments[-1]
                if source.startswith("ocidir://"):
                    layout, _, at = source[len("ocidir://"):].rpartition("@")
                    if not os.path.isfile(os.path.join(layout, "blobs", "sha256", at.split(":", 1)[1])):
                        return 1, "", "not in the layout"
                    if self.attached_ok:
                        self.tags[target] = [at]
                    return 0, "", ""
                if self.copies_ok:
                    self.tags[target] = [source.split("@", 1)[1]]
                    if "--digest-tags" in arguments:
                        base = target.rsplit(":", 1)[0]
                        for suffix in (".sig", ".att"):
                            at = self.resolve("%s:sha256-%s%s" % (SOURCE, digest.split(":", 1)[1], suffix))
                            self.tags["%s:sha256-%s%s" % (base, digest.split(":", 1)[1], suffix)] = [at]
                return 0, "", ""
            return 0, "", ""

        def regctl_bytes(self, *arguments):
            self.calls.append(("regctl",) + arguments)
            if arguments[:2] == ("manifest", "get") and "@" in arguments[2]:
                at = arguments[2].split("@", 1)[1]
            elif arguments[:2] == ("blob", "get"):
                at = arguments[3]
            else:
                return 1, b"", "not by digest"
            if at not in blobs:
                return 1, b"", "not found"
            return 0, blobs[at] + (b" " if at in self.lying else b""), ""

        def digest(self, reference):
            self.calls.append(("regctl", "image", "digest", reference))
            return self.resolve(reference)

        def cosign_(self, *arguments):
            self.calls.append(("cosign",) + arguments)
            if arguments[0] == "verify" and "--help" in arguments:
                return 0, "--new-bundle-format", ""
            identity = arguments[arguments.index("--certificate-identity") + 1]
            kind = KIND_SIGNATURES if arguments[0] == "verify" else KIND_ATTESTATIONS
            if "--local-image" in arguments:
                body = layout_manifest(arguments[-1], kind)
            else:
                # cosign given the registry fetches the tag itself: a read of its own.
                repository, at = arguments[-1].split("@", 1)
                body = blobs.get(self.resolve("%s:sha256-%s%s" % (repository, at.split(":", 1)[1],
                                                                  ".sig" if kind == KIND_SIGNATURES else ".att")))
            try:
                layers = go_fold(json.loads(body)).get("layers") or []
            except (TypeError, ValueError):
                layers = []
            for found in layers:
                annotations = found.get("annotations") or {}
                certificate = PEM_CERTIFICATE.match(annotations.get(SIG_CERTIFICATE) or "")
                verifies = annotations.get("test/verifies")
                if verifies == identity or verifies == "yes" and certificate and identity in rf.sigstore_certificate.uris(
                        base64.b64decode("".join(certificate.group(1).split()))):
                    return 0, json.dumps([{"critical": {"image": {"docker-manifest-digest": digest}}}]), ""
            return 1, "", "Error: no matching signatures"

    environ = {"HM_CHANNEL_VERSION": version, "DOCKERHUB_USERNAME": "progressiverobot", "DOCKERHUB_TOKEN": "dckr_pat_secret",
               "QUAY_ROBOT_USER": "hmailserver+ci", "QUAY_ROBOT_TOKEN": "quay-secret"}
    held = rf.release_version
    held_say = globals()["say"]

    def job(fake, name):
        """The job's exit code, or what it refused."""
        globals()["say"] = lambda line="": None
        try:
            return registry_mirror(os.path.join(work, name), environ, tools_for=lambda config: fake)
        except Refused as error:
            return "refused: %s" % error
        finally:
            globals()["say"] = held_say

    try:
        rf.release_version = lambda environ, fix: (version, "HM_CHANNEL_VERSION")
        fake = FakeTools()
        code = job(fake, "a")
        out = os.path.join(work, "a")
        expect("the job passes, the image signed and attested as the release's pipeline does (%s)" % code, code == 0)
        record = {}
        if code == 0:
            with open(os.path.join(out, "mirror.json"), encoding="utf-8") as f:
                record = json.load(f)
        copies = [c for c in fake.calls if c[:3] == ("regctl", "image", "copy") and not c[-2].startswith("ocidir://")]
        expect("verified, then copied by digest to both, each tag the source has, and nothing beside it unverified",
               code == 0 and [c[-1] for c in copies] == [
                   "docker.io/progressiverobot/hmailserver:9.9.9", "docker.io/progressiverobot/hmailserver:9.9",
                   "docker.io/progressiverobot/hmailserver:latest", "quay.io/hmailserver/hmailserver:9.9.9",
                   "quay.io/hmailserver/hmailserver:9.9", "quay.io/hmailserver/hmailserver:latest"]
               and all(c[3:] == ("%s@%s" % (SOURCE, digest), c[-1]) for c in copies))
        first_copy = fake.calls.index(copies[0])
        expect("  ... the source verified (signature and attestation) before any login or copy",
               [c[1] for c in fake.calls[:first_copy] if c[0] == "cosign"] == ["verify", "verify", "verify",
                                                                              "verify-attestation"]
               and fake.calls.index(next(c for c in fake.calls if c[1:3] == ("registry", "login"))) > max(
                   i for i, c in enumerate(fake.calls[:first_copy]) if c[0] == "cosign"))
        expect("  ... the credentials only on regctl's standard input",
               not any("dckr_pat_secret" in " ".join(c) or "quay-secret" in " ".join(c) for c in fake.calls))
        verifications = [c for c in fake.calls if c[0] == "cosign" and "--help" not in c]
        expect("  ... cosign given the layout the manifests were fetched into, never the registry",
               verifications and all("--local-image" in c and os.path.dirname(c[-1]) == os.path.join(out, "verified")
                                     for c in verifications))
        hexdigest = digest.split(":", 1)[1]
        resolved = [c[-1] for c in fake.calls if c[1:3] == ("image", "digest")]
        expect("  ... the source's .sig and .att tags each resolved once, and every manifest read by its digest",
               resolved.count("%s:sha256-%s.sig" % (SOURCE, hexdigest)) == 1
               and resolved.count("%s:sha256-%s.att" % (SOURCE, hexdigest)) == 1
               and all("@" in c[3] for c in fake.calls if c[1:3] == ("manifest", "get")))
        kinds = []
        if os.path.isfile(os.path.join(out, "verified", "source", "index.json")):
            with open(os.path.join(out, "verified", "source", "index.json"), encoding="utf-8") as f:
                kinds = [m["annotations"][KIND] for m in json.load(f)["manifests"]]
        expect("  ... the layout is cosign's: index.json names the image, its signatures and its attestations",
               kinds == [KIND_IMAGE, KIND_SIGNATURES, KIND_ATTESTATIONS])
        attached = [c for c in fake.calls if c[:3] == ("regctl", "image", "copy") and c[-2].startswith("ocidir://")]
        expect("  ... the verified signature and attestation copied out of the layout to each destination, after the image",
               [(c[-2].split("@")[1], c[-1]) for c in attached] == [
                   (record.get("signature"), "docker.io/progressiverobot/hmailserver:sha256-%s.sig" % hexdigest),
                   (record.get("attestation"), "docker.io/progressiverobot/hmailserver:sha256-%s.att" % hexdigest),
                   (record.get("signature"), "quay.io/hmailserver/hmailserver:sha256-%s.sig" % hexdigest),
                   (record.get("attestation"), "quay.io/hmailserver/hmailserver:sha256-%s.att" % hexdigest)]
               and fake.calls.index(attached[0]) > fake.calls.index(copies[2]))
        expect("  ... and the image verified again at each destination, from there",
               sum(1 for c in verifications if c[1] == "verify" and c[-1].endswith("destination-0")) == 1
               and any(c[1:3] == ("manifest", "get") and c[3].startswith("docker.io/progressiverobot/hmailserver@")
                       for c in fake.calls))

        # A source whose .sig tag answers the signature that verified, then another: the
        # destination gets the one verified.
        others = manifest([layer(rid="1267388815")])
        fake = FakeTools(sig=[manifest([layer()]), others])
        code = job(fake, "a1")
        expect("a source's .sig that changes after it was verified: the destination gets the verified one (%s)" % code,
               code == 0 and fake.tags["docker.io/progressiverobot/hmailserver:sha256-%s.sig" % hexdigest] ==
               [manifest([layer()])])
        fake = FakeTools(sig=[manifest([layer(), layer(), layer(rid=None, uris=["https://example.org/other"])])])
        code = job(fake, "a2")
        expect("an image signed twice as this project's pipeline, and once by another identity, is copied",
               code == 0 and any(c[:3] == ("regctl", "image", "copy") for c in fake.calls))
        fake = FakeTools(moved=())
        job(fake, "b")
        expect("an older release: :<major>.<minor> and :latest left where the source has them",
               [c[-1].rsplit(":", 1)[1] for c in fake.calls if c[:3] == ("regctl", "image", "copy")
                and not c[-2].startswith("ocidir://")] == ["9.9.9", "9.9.9"])

        def hidden(key_extra):
            """A signature manifest whose "layers" hold one that verifies nothing but
            carries this project's id, and whose other key holds another project's that
            verifies: cosign 2.6.5 reads the second (Go's encoding/json), a reader of
            "layers" the first."""
            return manifest([layer(good=False)], ', %s: [%s]' % (key_extra, json.dumps(layer(rid="1267388815"))))

        index = put(json.dumps({"schemaVersion": 2, "mediaType": IMAGE_INDEX_TYPES[0], "manifests": [
            {"mediaType": IMAGE_MANIFEST_TYPES[0], "digest": manifest([layer(rid="1267388815")]), "size": 1,
             "platform": {"architecture": "amd64", "os": "linux"}}]}).encode())
        cases = [
            ("an unsigned image", FakeTools(signed=False), "does not verify as signed"),
            ("an image without its SBOM attested", FakeTools(attested=False), "SBOM attested"),
            ("an image without an attestation manifest", FakeTools(att=[None]), "the image has no attestation there"),
            ("a signature at the group's path without the project's id", FakeTools(sig=[manifest([layer(rid=None)])]),
             "signing certificate, at the group's path, records no repository id"),
            ("another project's id", FakeTools(sig=[manifest([layer(rid="1267388815")])]), "not 86730559"),
            # The layer first whose certificate holds the identity's bytes - in its key -
            # and this project's id, before the one that verified, naming another id.
            ("a signature added before the verified one, this project's id in its extensions and the identity in its key",
             FakeTools(sig=[manifest([layer(key=group.encode(), uris=["https://gitlab.com/someone/else"], good=False),
                                      layer(rid="1267388815")])]), "not 86730559"),
            ("  ... the one added naming the identity too",
             FakeTools(sig=[manifest([layer(good=False), layer(rid="1267388815")])]), "not 86730559"),
            ("  ... this project's id planted in the key of the one that verified",
             FakeTools(sig=[manifest([layer(rid="1267388815", key=b"\x00" * 7 + rf.sigstore_certificate.planted())])]),
             "not 86730559"),
            # The signature cosign verified where Go's encoding/json finds it and Python's
            # json does not, and one of this project's where Python looks: cosign 2.6.5
            # verifies a signature so hidden (a key-signed one, against registry:2).
            ("a signature under \"Layers\" beside \"layers\"", FakeTools(sig=[hidden('"Layers"')]), "in some case"),
            ("  ... under \"LAYERS\" alone", FakeTools(sig=[manifest([layer(rid="1267388815")], key="LAYERS")]),
             "not a field of v1.Manifest"),
            ("  ... under \"layers\" with a long s", FakeTools(sig=[hidden('"layer\\u017f"')]), "not ASCII"),
            ("  ... its certificate under a layer's \"Annotations\"", FakeTools(sig=[manifest([layer(good=False), dict(
                [m for m in layer(rid="1267388815").items() if m[0] != "annotations"],
                Annotations=layer(rid="1267388815")["annotations"])])]), "not a field of v1.Descriptor"),
            ("  ... an index at the signature's tag, whose linux/amd64 manifest cosign reads", FakeTools(sig=[index]),
             "not a field of v1.Manifest"),
            # A registry serving the tag as an index hands cosign the child that
            # "Manifests" names, and a reader of the body the "layers" beside it (cosign
            # 2.6.5 verifies it; registry:2 refuses to store it).
            ("  ... an index under \"MANIFESTS\" beside \"layers\"", FakeTools(sig=[manifest(
                [layer()], ', "MANIFESTS": [{"mediaType": "%s", "digest": "%s", "size": 1}]'
                % (IMAGE_MANIFEST_TYPES[0], manifest([layer(rid="1267388815")])))]), "not a field of v1.Manifest"),
            ("  ... \"layers\" twice", FakeTools(sig=[manifest([layer(rid="1267388815")], ', "layers": [%s]' % json.dumps(
                layer()))]), "in some case"),
            ("  ... a key cosign does not read", FakeTools(sig=[manifest([layer()], ', "layerz": []')]),
             "not a field of v1.Manifest"),
            ("  ... a certificate that is not a string", FakeTools(sig=[manifest([dict(layer(), annotations={
                SIG_CERTIFICATE: 1, "test/verifies": "yes"})])]), "an object of strings"),
            ("no signature whose certificate names the identity in a URI, the reader's",
             FakeTools(sig=[manifest([layer(key=group.encode(), uris=["https://gitlab.com/someone/else"], good=group)])]),
             "carries a certificate naming"),
            # The SBOM's attestation, held to the project's id as the signature is.
            ("an SBOM attested by another project's pipeline at the identity",
             FakeTools(att=[manifest([layer(rid="1267388815", attestation=True)])]),
             "attestation's certificate names the repository id 1267388815"),
            ("  ... at the group's path without the project's id", FakeTools(att=[manifest([layer(rid=None, attestation=True)])]),
             "attestation's certificate, at the group's path, records no repository id"),
            ("  ... this project's id planted in its key", FakeTools(att=[manifest([layer(
                rid="1267388815", key=b"\x00" * 7 + rf.sigstore_certificate.planted(), attestation=True)])]),
             "attestation's certificate names the repository id 1267388815"),
            ("  ... an attestation added beside this project's", FakeTools(att=[manifest([layer(attestation=True), layer(
                rid="1267388815", attestation=True, good=False)])]), "attestation's certificate names the repository id"),
            ("  ... under \"Layers\" beside \"layers\"", FakeTools(att=[manifest([layer(attestation=True, good=False)], (
                ', "Layers": [%s]' % json.dumps(layer(rid="1267388815", attestation=True))))]), "in some case"),
            # A registry that answers the .sig tag with the signature another project's
            # pipeline made at the identity, and then with this project's: one read, so
            # cosign and the reader both have the first.
            ("a .sig tag answering another project's signature, then this project's",
             FakeTools(sig=[manifest([layer(rid="1267388815")]), manifest([layer()])]), "not 86730559"),
            ("a .sig tag answering this project's signature that verifies nothing, then another project's that does",
             FakeTools(sig=[manifest([layer(good=False)]), manifest([layer(rid="1267388815")])]), "does not verify as signed"),
            ("a manifest answered by its digest that is not it", FakeTools(lying=(digest,)), "whose digest is"),
            ("a signature's blob that is not its digest",
             FakeTools(lying=(json.loads(blobs[manifest([layer()])])["layers"][0]["digest"],)), "that is not it"),
            ("a destination that does not hold the digest after the copy", FakeTools(copies_ok=False), "after the copy"),
            ("a destination that does not hold the verified signature after the copy (the source's changed since)",
             FakeTools(sig=[manifest([layer()]), manifest([layer(rid="1267388815")])], attached_ok=False),
             "not the signature verified")]
        for what, fake, needle in cases:
            globals()["say"] = lambda line="": None
            try:
                refused("refused: " + what, lambda: registry_mirror(os.path.join(work, "c"), environ,
                                                                   tools_for=lambda config: fake), needle)
            finally:
                globals()["say"] = held_say
            if "destination" not in what:
                expect("  ... and nothing was logged in to or copied",
                       not any(c[1:3] in (("registry", "login"), ("image", "copy")) for c in fake.calls))
        if cosign:
            real_cosign_cases(expect, cosign, work)
        else:
            print("skip  the layouts against a real cosign: no --cosign given")
    finally:
        rf.release_version = held
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
    parser.add_argument("--cosign", help="with --self-test: a cosign to verify the layouts with")
    parser.add_argument("--out", default="registry-mirror")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test(arguments.cosign)
    try:
        return registry_mirror(os.path.abspath(arguments.out))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
