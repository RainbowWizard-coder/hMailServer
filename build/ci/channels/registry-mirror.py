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
     its digest, and verified before anything is sent: cosign verify of the image by
     that digest as this project's pipeline for the tag (either path a server trusts),
     cosign verify-attestation of its CycloneDX SBOM as the same identity, and the
     project's id in the signing certificate (read from the signature's manifest, as
     for the release's files: required at the group's path, and refused if another).
  4. For each configured destination: regctl image copy of the source BY ITS DIGEST,
     with --digest-tags (cosign keeps the signature and the attestation beside the
     image as sha256-<digest>.sig and .att, for the index and for each platform's
     manifest) and --referrers, to :<version>; and to :<major>.<minor> and :latest only
     where the source's own tag of that name is the same digest, so an older release
     mirrored late never moves them back. Then each tag at the destination is held to
     the digest, and the image there verified again with cosign, anonymously, as a
     user would pull and verify it.

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


def verify_image(tools, repository, digest, version, identities, issuer):
    """(identity, repository id) of an image that verifies, by digest, as one of the
    identities, with its SBOM attested by the same; Refused otherwise."""
    reference = "%s@%s" % (repository, digest)
    flags = verify_flags(tools)
    identity, said = None, ""
    for candidate in identities:
        code, out, err = tools.cosign_("verify", *flags, "--certificate-identity", candidate,
                                       "--certificate-oidc-issuer", issuer, reference)
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
    code, out, err = tools.cosign_("verify-attestation", *flags, "--type", ATTESTATION_TYPE, "--certificate-identity",
                                   identity, "--certificate-oidc-issuer", issuer, reference)
    if code != 0:
        raise Refused("%s has no %s SBOM attested by %s: nothing was copied.\n  cosign said: %s"
                      % (reference, ATTESTATION_TYPE, identity, (err or out).splitlines()[-1] if (err or out) else ""))
    recorded = signature_repository_id(tools, repository, digest, identity)
    if recorded is not None and recorded != rf.PROJECT_ID:
        raise Refused("the image's signing certificate names the repository id %s, not %s" % (recorded, rf.PROJECT_ID))
    if recorded is None and identity.startswith(rf.GROUP_REPOSITORY + "//"):
        raise Refused("the image's signing certificate, at the group's path, records no repository id")
    return identity, recorded


def signature_repository_id(tools, repository, digest, identity):
    """The repository id in the certificate of the image's signature that names
    identity, read from cosign's signature manifest (sha256-<hex>.sig)."""
    code, out, err = tools.regctl_("manifest", "get", "%s:sha256-%s.sig" % (repository, digest.split(":", 1)[1]),
                                   "--format", "raw-body")
    if code != 0:
        raise Refused("the image's signature manifest could not be read: %s" % err[-300:])
    try:
        layers = json.loads(out).get("layers") or []
    except ValueError:
        raise Refused("the image's signature manifest is not JSON")
    for layer in layers:
        text = (layer.get("annotations") or {}).get(SIG_CERTIFICATE) or ""
        body = "".join(line for line in text.splitlines() if line and not line.startswith("-----"))
        try:
            der = base64.b64decode(body)
        except ValueError:
            continue
        if identity.encode("utf-8") in der:
            try:
                return rf.repository_id(der)
            except ValueError as error:
                raise Refused("the image's signing certificate could not be read: %s" % error)
    raise Refused("no signature of the image carries a certificate naming %s" % identity)


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
        identity, recorded = verify_image(tools, source, digest, version, identities, issuer)
        say("verified: %s@%s, signed and its SBOM attested as %s (repository id: %s)"
            % (source, digest, identity, recorded or "none recorded"))
        tags = mirror_tags(tools, source, digest, version)

        record = {"version": version, "source": source, "digest": digest, "identity": identity, "destinations": {}}
        for destination, repository in destinations:
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
                code, _, err = tools.regctl_("image", "copy", "--digest-tags", "--referrers",
                                             "%s@%s" % (source, digest), "%s:%s" % (repository, tag))
                if code != 0:
                    raise Refused("the copy to %s:%s failed: %s" % (repository, tag, err[-400:]))
            for tag in tags:
                held = tools.digest("%s:%s" % (repository, tag))
                if held != digest:
                    raise Refused("%s:%s is %s after the copy, not %s" % (repository, tag, held, digest))
            tools.regctl_("registry", "logout", host)
            verify_image(tools, repository, digest, version, [identity], issuer)
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

    # The job against a registry and a cosign in memory.
    version = "9.9.9"
    digest = "sha256:" + "d" * 64
    group = rf.identities(version)[1]

    class FakeTools:
        def __init__(self, signed=True, attested=True, rid="86730559", moved=("9.9", "latest"), copies_ok=True):
            self.calls, self.signed, self.attested, self.rid, self.moved = [], signed, attested, rid, moved
            self.copies_ok = copies_ok
            self.images = {}

        def regctl_(self, *arguments, stdin=None):
            self.calls.append(("regctl",) + arguments + (("<stdin>",) if stdin else ()))
            if arguments[:2] == ("manifest", "get"):
                # The identity stands where a certificate's subject alternative name
                # would; the id, when there is one, in the extensions.
                cert = rf.test_certificate(self.rid, key=group.encode())
                pem = "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n" % base64.b64encode(cert).decode()
                return 0, json.dumps({"layers": [{"annotations": {SIG_CERTIFICATE: pem}}]}), ""
            if arguments[:2] == ("image", "copy"):
                if self.copies_ok:
                    self.images[arguments[-1]] = digest
                return 0, "", ""
            return 0, "", ""

        def cosign_(self, *arguments):
            self.calls.append(("cosign",) + arguments)
            ok = self.signed if arguments[0] == "verify" else self.attested
            if arguments[0] == "verify" and "--help" in arguments:
                return 0, "--new-bundle-format", ""
            identity = arguments[arguments.index("--certificate-identity") + 1]
            if ok and identity == group:
                return 0, json.dumps([{"critical": {"image": {"docker-manifest-digest": digest}}}]), ""
            return 1, "", "Error: no matching signatures"

        def digest(self, reference):
            if reference == "%s:%s" % (SOURCE, version):
                return digest
            if reference.startswith(SOURCE + ":"):
                return digest if reference.rsplit(":", 1)[1] in self.moved else "sha256:" + "e" * 64
            return self.images.get(reference)

    environ = {"HM_CHANNEL_VERSION": version, "DOCKERHUB_USERNAME": "progressiverobot", "DOCKERHUB_TOKEN": "dckr_pat_secret",
               "QUAY_ROBOT_USER": "hmailserver+ci", "QUAY_ROBOT_TOKEN": "quay-secret"}
    held = rf.release_version
    work = tempfile.mkdtemp(prefix="hm-mirror-test-")
    try:
        rf.release_version = lambda environ, fix: (version, "HM_CHANNEL_VERSION")
        fake = FakeTools()
        held_say = globals()["say"]
        globals()["say"] = lambda line="": None
        try:
            code = registry_mirror(os.path.join(work, "a"), environ, tools_for=lambda config: fake)
        finally:
            globals()["say"] = held_say
        copies = [c for c in fake.calls if c[:3] == ("regctl", "image", "copy")]
        expect("verified, then copied by digest with --digest-tags and --referrers to both, each tag the source has",
               code == 0 and [c[-1] for c in copies] == [
                   "docker.io/progressiverobot/hmailserver:9.9.9", "docker.io/progressiverobot/hmailserver:9.9",
                   "docker.io/progressiverobot/hmailserver:latest", "quay.io/hmailserver/hmailserver:9.9.9",
                   "quay.io/hmailserver/hmailserver:9.9", "quay.io/hmailserver/hmailserver:latest"]
               and all(c[3:5] == ("--digest-tags", "--referrers") and c[5] == "%s@%s" % (SOURCE, digest) for c in copies))
        first_copy = fake.calls.index(copies[0])
        expect("  ... the source verified (signature and attestation) before any login or copy",
               [c[1] for c in fake.calls[:first_copy] if c[0] == "cosign"] == ["verify", "verify", "verify",
                                                                              "verify-attestation"]
               and fake.calls.index(next(c for c in fake.calls if c[1:3] == ("registry", "login"))) > max(
                   i for i, c in enumerate(fake.calls[:first_copy]) if c[0] == "cosign"))
        expect("  ... the credentials only on regctl's standard input",
               not any("dckr_pat_secret" in " ".join(c) or "quay-secret" in " ".join(c) for c in fake.calls))
        expect("  ... and the image verified again at each destination",
               sum(1 for c in fake.calls if c[0] == "cosign" and c[1] == "verify" and "--help" not in c
                   and c[-1].startswith("docker.io/progressiverobot/hmailserver@")) == 1)
        fake = FakeTools(moved=())
        globals()["say"] = lambda line="": None
        try:
            registry_mirror(os.path.join(work, "b"), environ, tools_for=lambda config: fake)
        finally:
            globals()["say"] = held_say
        expect("an older release: :<major>.<minor> and :latest left where the source has them",
               [c[-1].rsplit(":", 1)[1] for c in fake.calls if c[:3] == ("regctl", "image", "copy")] == ["9.9.9", "9.9.9"])
        for what, fake, needle in (
                ("an unsigned image", FakeTools(signed=False), "does not verify as signed"),
                ("an image without its SBOM attested", FakeTools(attested=False), "SBOM attested"),
                ("a signature at the group's path without the project's id", FakeTools(rid=None), "records no repository id"),
                ("another project's id", FakeTools(rid="1267388815"), "not 86730559"),
                ("a destination that does not hold the digest after the copy", FakeTools(copies_ok=False), "after the copy")):
            globals()["say"] = lambda line="": None
            try:
                refused("refused: " + what, lambda: registry_mirror(os.path.join(work, "c"), environ,
                                                                   tools_for=lambda config: fake), needle)
            finally:
                globals()["say"] = held_say
            if what != "a destination that does not hold the digest after the copy":
                expect("  ... and nothing was logged in to or copied",
                       not any(c[1:3] in (("registry", "login"), ("image", "copy")) for c in fake.calls))
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
    parser.add_argument("--out", default="registry-mirror")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    try:
        return registry_mirror(os.path.abspath(arguments.out))
    except Refused as error:
        say("REFUSED: %s" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
