#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A Sigstore of the channel jobs' own, for their tests: what
hmailserver/test/RegressionTests/Shared/FakeSigstore.cs is to the server's verifier,
for cosign.

It is a certificate authority shaped like Fulcio (a root, an intermediate that may
issue for code signing, and ten-minute leaves naming a pipeline's identity in the
subject alternative name, with Fulcio's issuer, repository and repository-id
extensions), a certificate transparency log shaped like Fulcio's (an SCT embedded in
each leaf, over the precertificate, as RFC 6962 has it), and a transparency log shaped
like Rekor (a P-256 key whose id is the SHA-256 of its DER, a hashedrekord 0.0.1
entry, a signed entry timestamp, and an inclusion proof to a signed checkpoint). It
writes the bundle `cosign sign-blob --new-bundle-format` writes, and a Sigstore
TrustedRoot naming its own keys, which cosign verify-blob takes with --trusted-root.
So the channel jobs' verification (build/ci/channels/release-files.py) is proven with
the very cosign the jobs run, against a file that verifies and against every way one
can fail to - without a signing identity, and with no network.

It needs the cryptography package; the jobs never import it. Nothing it makes is
trusted anywhere but in a command that is handed its trusted root, and
release-files.py refuses that argument in a CI job.

  python3 build/ci/channels/fake-sigstore.py --self-test [--cosign PATH]

without --cosign checks the arithmetic alone (the Merkle path, the encodings); with it,
that cosign verifies a good bundle and refuses a tampered file, another identity,
another issuer and a certificate from another authority.
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except ImportError:  # the self-test says so; the jobs never load this module
    x509 = None

CODE_SIGNING = "1.3.6.1.5.5.7.3.3"
# Fulcio's extensions (https://github.com/sigstore/fulcio, docs/oid-info.md, as the
# server's SigstoreVerifier.cpp and FakeSigstore.cs name them).
OID_ISSUER_V1 = "1.3.6.1.4.1.57264.1.1"
OID_ISSUER = "1.3.6.1.4.1.57264.1.8"
OID_REPOSITORY = "1.3.6.1.4.1.57264.1.12"
OID_REPOSITORY_ID = "1.3.6.1.4.1.57264.1.15"
# RFC 6962's embedded SCT list.
OID_SCT_LIST = "1.3.6.1.4.1.11129.2.4.2"


def der_utf8(text):
    data = text.encode("utf-8")
    if len(data) < 128:
        return bytes([0x0C, len(data)]) + data
    return bytes([0x0C, 0x81, len(data)]) + data


def der_octets(data):
    if len(data) < 128:
        return bytes([0x04, len(data)]) + data
    if len(data) < 256:
        return bytes([0x04, 0x81, len(data)]) + data
    return bytes([0x04, 0x82]) + struct.pack(">H", len(data)) + data


def spki(key):
    return key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


def pem(label, der):
    body = base64.b64encode(der).decode("ascii")
    return "-----BEGIN %s-----\n%s-----END %s-----\n" % (label, "".join(body[i:i + 64] + "\n" for i in range(0, len(body), 64)),
                                                         label)


def b64(data):
    return base64.b64encode(data).decode("ascii")


def utc(moment):
    return moment.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------- the log's arithmetic (RFC 9162)

def leaf_hash(data):
    return hashlib.sha256(b"\x00" + data).digest()


def node_hash(left, right):
    return hashlib.sha256(b"\x01" + left + right).digest()


def largest_power_of_two_below(n):
    k = 1
    while k * 2 < n:
        k *= 2
    return k


def merkle_root(leaves):
    if len(leaves) == 1:
        return leaves[0]
    k = largest_power_of_two_below(len(leaves))
    return node_hash(merkle_root(leaves[:k]), merkle_root(leaves[k:]))


def inclusion_path(leaves, index):
    if len(leaves) == 1:
        return []
    k = largest_power_of_two_below(len(leaves))
    if index < k:
        return inclusion_path(leaves[:k], index) + [merkle_root(leaves[k:])]
    return inclusion_path(leaves[k:], index - k) + [merkle_root(leaves[:k])]


def root_from_path(leaf, index, size, path):
    """RFC 9162 2.1.3.2: the root an inclusion path leads to, for the self-test."""
    fn, sn, r = index, size - 1, leaf
    for p in path:
        if sn == 0:
            return None
        if fn % 2 == 1 or fn == sn:
            r = node_hash(p, r)
            while fn % 2 == 0 and fn != 0:
                fn >>= 1
                sn >>= 1
        else:
            r = node_hash(r, p)
        fn >>= 1
        sn >>= 1
    return r if sn == 0 else None


class FakeSigstore:
    def __init__(self, name="sigstore.test", now=None):
        if x509 is None:
            raise RuntimeError("the cryptography package is not installed")
        self.now = now or datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
        self.name = name
        self.root_key = ec.generate_private_key(ec.SECP384R1())
        self.intermediate_key = ec.generate_private_key(ec.SECP384R1())
        self.log_key = ec.generate_private_key(ec.SECP256R1())
        self.ct_key = ec.generate_private_key(ec.SECP256R1())
        self.log_id = hashlib.sha256(spki(self.log_key)).digest()
        self.ct_id = hashlib.sha256(spki(self.ct_key)).digest()
        self.valid_from = self.now - datetime.timedelta(days=365)

        root_name = x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, name),
                               x509.NameAttribute(NameOID.COMMON_NAME, name)])
        self.root = (x509.CertificateBuilder()
                     .subject_name(root_name).issuer_name(root_name)
                     .public_key(self.root_key.public_key())
                     .serial_number(x509.random_serial_number())
                     .not_valid_before(self.valid_from).not_valid_after(self.now + datetime.timedelta(days=5 * 365))
                     .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                     .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
                     .add_extension(x509.SubjectKeyIdentifier.from_public_key(self.root_key.public_key()), critical=False)
                     .sign(self.root_key, hashes.SHA384()))
        intermediate_name = x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, name),
                                       x509.NameAttribute(NameOID.COMMON_NAME, name + "-intermediate")])
        self.intermediate = (x509.CertificateBuilder()
                             .subject_name(intermediate_name).issuer_name(root_name)
                             .public_key(self.intermediate_key.public_key())
                             .serial_number(x509.random_serial_number())
                             .not_valid_before(self.valid_from + datetime.timedelta(days=1))
                             .not_valid_after(self.now + datetime.timedelta(days=4 * 365))
                             .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
                             .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False),
                                            critical=True)
                             .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), critical=False)
                             .add_extension(x509.SubjectKeyIdentifier.from_public_key(self.intermediate_key.public_key()),
                                            critical=False)
                             .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(self.root_key.public_key()),
                                            critical=False)
                             .sign(self.root_key, hashes.SHA384()))

    def trusted_root(self):
        """A Sigstore TrustedRoot (protobuf-specs, JSON) naming this authority, this CT
        log and this transparency log, and nothing else."""
        start = utc(self.valid_from)
        return {
            "mediaType": "application/vnd.dev.sigstore.trustedroot+json;version=0.1",
            "tlogs": [{
                "baseUrl": "https://rekor." + self.name,
                "hashAlgorithm": "SHA2_256",
                "publicKey": {"rawBytes": b64(spki(self.log_key)), "keyDetails": "PKIX_ECDSA_P256_SHA_256",
                              "validFor": {"start": start}},
                "logId": {"keyId": b64(self.log_id)},
            }],
            "certificateAuthorities": [{
                "subject": {"organization": self.name, "commonName": self.name},
                "uri": "https://fulcio." + self.name,
                "certChain": {"certificates": [
                    {"rawBytes": b64(self.intermediate.public_bytes(serialization.Encoding.DER))},
                    {"rawBytes": b64(self.root.public_bytes(serialization.Encoding.DER))},
                ]},
                "validFor": {"start": start},
            }],
            "ctlogs": [{
                "baseUrl": "https://ctfe." + self.name,
                "hashAlgorithm": "SHA2_256",
                "publicKey": {"rawBytes": b64(spki(self.ct_key)), "keyDetails": "PKIX_ECDSA_P256_SHA_256",
                              "validFor": {"start": start}},
                "logId": {"keyId": b64(self.ct_id)},
            }],
            "timestampAuthorities": [],
        }

    def write_trusted_root(self, path):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.trusted_root(), f, indent=1)
        return path

    def _sct(self, tbs, timestamp_ms):
        """An RFC 6962 v1 SCT over the precertificate whose TBSCertificate is tbs, as
        the extension's TLS-encoded SignedCertificateTimestampList."""
        issuer_key_hash = hashlib.sha256(spki(self.intermediate_key)).digest()
        extensions = b""
        signed = (bytes([0, 0]) + struct.pack(">Q", timestamp_ms) + struct.pack(">H", 1) + issuer_key_hash
                  + struct.pack(">I", len(tbs))[1:] + tbs + struct.pack(">H", len(extensions)) + extensions)
        signature = self.ct_key.sign(signed, ec.ECDSA(hashes.SHA256()))
        sct = (bytes([0]) + self.ct_id + struct.pack(">Q", timestamp_ms) + struct.pack(">H", len(extensions)) + extensions
               + bytes([4, 3]) + struct.pack(">H", len(signature)) + signature)
        listed = struct.pack(">H", len(sct)) + sct
        return struct.pack(">H", len(listed)) + listed

    def bundle(self, content, identity, issuer="https://gitlab.com", repository=None, repository_id=None,
               signed_at=None, sign_other=None, other_log_key=None, no_code_signing=False, log_index=2620945251):
        """The bundle cosign sign-blob --new-bundle-format writes for content, signed as
        identity; or the deviation the arguments ask for."""
        signed_at = signed_at or (self.now - datetime.timedelta(hours=1))
        digest = hashlib.sha256(sign_other if sign_other is not None else content).digest()
        leaf_key = ec.generate_private_key(ec.SECP256R1())
        serial = x509.random_serial_number()

        def leaf(with_sct=None):
            builder = (x509.CertificateBuilder()
                       .subject_name(x509.Name([])).issuer_name(self.intermediate.subject)
                       .public_key(leaf_key.public_key()).serial_number(serial)
                       .not_valid_before(signed_at - datetime.timedelta(minutes=5))
                       .not_valid_after(signed_at + datetime.timedelta(minutes=5))
                       .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False),
                                      critical=True))
            if not no_code_signing:
                builder = builder.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), critical=False)
            builder = (builder
                       .add_extension(x509.SubjectKeyIdentifier.from_public_key(leaf_key.public_key()), critical=False)
                       .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(self.intermediate_key.public_key()),
                                      critical=False)
                       .add_extension(x509.SubjectAlternativeName([x509.UniformResourceIdentifier(identity)]),
                                      critical=True)
                       .add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier(OID_ISSUER_V1),
                                                                 issuer.encode("ascii")), critical=False)
                       .add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier(OID_ISSUER), der_utf8(issuer)),
                                      critical=False))
            if repository is not None:
                builder = builder.add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier(OID_REPOSITORY),
                                                                           der_utf8(repository)), critical=False)
            if repository_id is not None:
                builder = builder.add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier(OID_REPOSITORY_ID),
                                                                           der_utf8(repository_id)), critical=False)
            if with_sct is not None:
                builder = builder.add_extension(x509.UnrecognizedExtension(x509.ObjectIdentifier(OID_SCT_LIST),
                                                                           der_octets(with_sct)), critical=False)
            return builder.sign(self.intermediate_key, hashes.SHA256())

        # The precertificate's TBSCertificate is the final one's without the SCT
        # extension; DER makes the same bytes from the same fields, so the certificate
        # made without it has exactly that TBSCertificate.
        tbs = leaf().tbs_certificate_bytes
        certificate = leaf(self._sct(tbs, int(signed_at.timestamp() * 1000)))
        leaf_der = certificate.public_bytes(serialization.Encoding.DER)
        leaf_pem = pem("CERTIFICATE", leaf_der)

        from cryptography.hazmat.primitives.asymmetric.utils import Prehashed
        signature = leaf_key.sign(digest, ec.ECDSA(Prehashed(hashes.SHA256())))
        integrated = int(signed_at.timestamp())

        body = ('{"apiVersion":"0.0.1","kind":"hashedrekord","spec":{"data":{"hash":{"algorithm":"sha256","value":"%s"}},'
                '"signature":{"content":"%s","publicKey":{"content":"%s"}}}}'
                % (digest.hex(), b64(signature), b64(leaf_pem.encode("ascii")))).encode("ascii")
        body64 = b64(body)
        log_signer = other_log_key or self.log_key

        canonical = ('{"body":"%s","integratedTime":%d,"logID":"%s","logIndex":%d}'
                     % (body64, integrated, self.log_id.hex(), log_index)).encode("ascii")
        entry_timestamp = log_signer.sign(canonical, ec.ECDSA(hashes.SHA256()))

        # A small tree with the entry at a chosen leaf, as FakeSigstore.cs makes one.
        size, position = 7, 5
        filler = random.Random(12345)
        leaves = [leaf_hash(body) if i == position else bytes(filler.getrandbits(8) for _ in range(32))
                  for i in range(size)]
        root = merkle_root(leaves)
        path = inclusion_path(leaves, position)
        note = "rekor.test - 1\n%d\n%s\n" % (size, b64(root))
        note_signature = log_signer.sign(note.encode("utf-8"), ec.ECDSA(hashes.SHA256()))
        envelope = note + "\n— rekor.test " + b64(self.log_id[:4] + note_signature) + "\n"

        return {
            "mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json",
            "verificationMaterial": {
                "certificate": {"rawBytes": b64(leaf_der)},
                "tlogEntries": [{
                    "logIndex": str(log_index),
                    "logId": {"keyId": b64(self.log_id)},
                    "kindVersion": {"kind": "hashedrekord", "version": "0.0.1"},
                    "integratedTime": str(integrated),
                    "inclusionPromise": {"signedEntryTimestamp": b64(entry_timestamp)},
                    "inclusionProof": {"logIndex": str(position), "rootHash": b64(root), "treeSize": str(size),
                                       "hashes": [b64(h) for h in path], "checkpoint": {"envelope": envelope}},
                    "canonicalizedBody": body64,
                }],
                "timestampVerificationData": {},
            },
            "messageSignature": {"messageDigest": {"algorithm": "SHA2_256", "digest": b64(digest)},
                                 "signature": b64(signature)},
        }

    def write_bundle(self, path, content, identity, **kwargs):
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.bundle(content, identity, **kwargs), f)
        return path


# ---------------------------------------------------------------- self-test

def self_test(cosign=None):
    failures = []

    def expect(what, condition):
        print("%s  %s" % ("ok  " if condition else "FAIL", what))
        if not condition:
            failures.append(what)

    # The Merkle arithmetic against RFC 9162's own verification algorithm, every
    # position of every size up to nine.
    good = True
    for size in range(1, 10):
        leaves = [hashlib.sha256(bytes([size, i])).digest() for i in range(size)]
        for index in range(size):
            if root_from_path(leaves[index], index, size, inclusion_path(leaves, index)) != merkle_root(leaves):
                good = False
    expect("every inclusion path of trees of 1 to 9 leaves leads to the root", good)
    expect("a DER UTF8String of 200 bytes takes the long form", der_utf8("a" * 200)[:3] == bytes([0x0C, 0x81, 200]))

    if x509 is None:
        print("skip  the bundles: the cryptography package is not installed")
        return report(failures)

    fake = FakeSigstore()
    content = b"hMailServer channel fixture\n"
    identity = "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v9.9.9"
    bundle = fake.bundle(content, identity, repository="https://gitlab.com/hmailserver/hmailserver",
                         repository_id="86730559")
    certificate = x509.load_der_x509_certificate(base64.b64decode(bundle["verificationMaterial"]["certificate"]["rawBytes"]))
    expect("the leaf names the identity", certificate.extensions.get_extension_for_class(
        x509.SubjectAlternativeName).value.get_values_for_type(x509.UniformResourceIdentifier) == [identity])
    expect("the leaf carries the repository id as a DER UTF8String",
           certificate.extensions.get_extension_for_oid(x509.ObjectIdentifier(OID_REPOSITORY_ID)).value.value
           == der_utf8("86730559"))
    expect("the leaf carries an SCT list", certificate.extensions.get_extension_for_oid(
        x509.ObjectIdentifier(OID_SCT_LIST)) is not None)
    entry = bundle["verificationMaterial"]["tlogEntries"][0]
    proof = entry["inclusionProof"]
    expect("the inclusion proof leads to its root", root_from_path(
        leaf_hash(base64.b64decode(entry["canonicalizedBody"])), int(proof["logIndex"]), int(proof["treeSize"]),
        [base64.b64decode(h) for h in proof["hashes"]]) == base64.b64decode(proof["rootHash"]))

    if not cosign:
        print("skip  cosign's verdicts: no --cosign given")
        return report(failures)

    work = tempfile.mkdtemp(prefix="hm-fake-sigstore-")
    try:
        root = fake.write_trusted_root(os.path.join(work, "trusted_root.json"))
        path = os.path.join(work, "file")
        with open(path, "wb") as f:
            f.write(content)

        def verify(bundle_path, file_path=path, who=identity, issuer="https://gitlab.com", trusted=root):
            answer = subprocess.run([cosign, "verify-blob", "--new-bundle-format", "--trusted-root", trusted,
                                     "--bundle", bundle_path, "--certificate-identity", who,
                                     "--certificate-oidc-issuer", issuer, file_path],
                                    capture_output=True, text=True)
            return answer.returncode, (answer.stderr + answer.stdout).strip().splitlines()[-1:] or [""]

        good_bundle = fake.write_bundle(os.path.join(work, "good.bundle"), content, identity,
                                        repository="https://gitlab.com/hmailserver/hmailserver", repository_id="86730559")
        code, said = verify(good_bundle)
        expect("cosign verifies the fixture's bundle (%s)" % said[0][:90], code == 0)
        with open(os.path.join(work, "tampered"), "wb") as f:
            f.write(content + b"x")
        code, said = verify(good_bundle, os.path.join(work, "tampered"))
        expect("cosign refuses a file the bundle is not over (%s)" % said[0][:90], code != 0)
        code, said = verify(good_bundle, who=identity.replace("v9.9.9", "v9.9.8"))
        expect("cosign refuses another identity (%s)" % said[0][:90], code != 0)
        code, said = verify(good_bundle, issuer="https://token.actions.githubusercontent.com")
        expect("cosign refuses another issuer (%s)" % said[0][:90], code != 0)
        other = FakeSigstore("other.test")
        foreign = other.write_bundle(os.path.join(work, "foreign.bundle"), content, identity)
        code, said = verify(foreign)
        expect("cosign refuses a certificate from another authority (%s)" % said[0][:90], code != 0)
        forged = fake.write_bundle(os.path.join(work, "forged.bundle"), content, identity,
                                   other_log_key=ec.generate_private_key(ec.SECP256R1()))
        code, said = verify(forged)
        expect("cosign refuses a log entry the log did not sign (%s)" % said[0][:90], code != 0)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return report(failures)


def report(failures):
    print("")
    if failures:
        print("%d self-test case(s) failed" % len(failures))
        return 1
    print("Every self-test case passed.")
    return 0


# ---------------------------------------------------------------- a fixture release, served

PROJECT_API = "/api/v4/projects/86730559"


def make_fixture(directory, version, files, port, tamper=(), other_signer=(), group=True, extra=None):
    """A release v<version> of the given files (paths), each signed by this fake
    Sigstore as the project's tag pipeline, laid out as the feed, the releases API
    and the package registry serve it under http://127.0.0.1:<port>/, and the
    fixture.json release-files.py takes (HM_CHANNELS_FIXTURE). tamper: names whose
    bytes change after they are signed; other_signer: names signed by a branch's
    pipeline (a rehearsal), which no channel takes."""
    fake = FakeSigstore()
    base = "http://127.0.0.1:%d" % port
    repository = "https://gitlab.com/%s/hmailserver" % ("hmailserver" if group else "Progressiverobot")
    identity = repository + "//.gitlab-ci.yml@refs/tags/v" + version
    registry = os.path.join(directory, "site", PROJECT_API.strip("/"), "packages", "generic", "hmailserver", version)
    os.makedirs(registry, exist_ok=True)
    links = []
    for source in files:
        name = os.path.basename(source)
        with open(source, "rb") as f:
            content = f.read()
        signer = identity.replace("refs/tags/v" + version, "refs/heads/batch42") if name in other_signer else identity
        bundle = fake.bundle(content, signer, repository=repository, repository_id="86730559")
        if name in tamper:
            content += b"\nchanged after it was signed\n"
        with open(os.path.join(registry, name), "wb") as f:
            f.write(content)
        with open(os.path.join(registry, name + ".cosign.bundle"), "w", encoding="utf-8") as f:
            json.dump(bundle, f)
        for listed in (name, name + ".cosign.bundle"):
            links.append({"name": listed, "url": "https://gitlab.com%s/packages/generic/hmailserver/%s/%s"
                          % (PROJECT_API, version, listed), "link_type": "other"})
    release = {"tag_name": "v" + version, "name": "hMailServer " + version, "draft": False, "prerelease": False,
               "assets": {"links": links}}
    feed = os.path.join(directory, "site", "feed")
    os.makedirs(os.path.join(feed, "tags"), exist_ok=True)
    for path in (os.path.join(feed, "latest"), os.path.join(feed, "tags", "v" + version)):
        with open(path, "w", encoding="utf-8") as f:
            json.dump(release, f)
    releases = os.path.join(directory, "site", PROJECT_API.strip("/"), "releases")
    os.makedirs(releases, exist_ok=True)
    with open(os.path.join(releases, "v" + version), "w", encoding="utf-8") as f:
        json.dump(release, f)
    data = {"feed": base + "/feed/", "api": base + PROJECT_API, "trusted_root": fake.write_trusted_root(
        os.path.join(directory, "trusted_root.json"))}
    data.update(extra or {})
    with open(os.path.join(directory, "fixture.json"), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    return os.path.join(directory, "fixture.json")


def serve(directory, port):
    import functools
    import http.server
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=os.path.join(directory, "site"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    print("serving %s on http://127.0.0.1:%d/ (HM_CHANNELS_FIXTURE=%s)" % (directory, port,
                                                                          os.path.join(directory, "fixture.json")))
    sys.stdout.flush()
    server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--cosign", help="a cosign to prove the bundles with")
    commands = parser.add_subparsers(dest="command")
    f = commands.add_parser("fixture", help="a signed fixture release, served on the loopback until stopped")
    f.add_argument("--dir", required=True)
    f.add_argument("--version", required=True)
    f.add_argument("--port", type=int, default=8931)
    f.add_argument("--tamper", action="append", default=[], help="a file whose bytes change after it is signed")
    f.add_argument("--other-signer", action="append", default=[], help="a file signed by a branch's pipeline")
    f.add_argument("--personal", action="store_true", help="signed at the personal path, not the group's")
    f.add_argument("--set", action="append", default=[], help="KEY=VALUE for fixture.json (a channel's own target)")
    f.add_argument("--no-serve", action="store_true")
    f.add_argument("files", nargs="+")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test(arguments.cosign)
    if arguments.command == "fixture":
        extra = dict(item.split("=", 1) for item in arguments.set)
        path = make_fixture(os.path.abspath(arguments.dir), arguments.version, arguments.files, arguments.port,
                            set(arguments.tamper), set(arguments.other_signer), not arguments.personal, extra)
        print(path)
        if not arguments.no_serve:
            serve(os.path.abspath(arguments.dir), arguments.port)
        return 0
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
