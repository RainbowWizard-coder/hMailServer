#!/usr/bin/env python3
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A Fulcio certificate's fields, read from its extensions and from nowhere else: the
one reader of the release chain and the channels - build/ci/release-verify.sh,
build/ci/release-chain.py check-provenance, build/ci/channels/release-files.py and
registry-mirror.py - and the one build/ci/sigstore-certificate.sh, which
container-image-deb.sh runs in cosign's image where there is no Python, is held to.

Why never a search of the certificate's bytes for an OID, a value or an identity: a
certificate's public key is bytes its holder chooses - Fulcio issues certificates for
RSA keys, whose modulus is whatever its maker wants in it - so whoever can obtain a
certificate at this project's path could put this project's id, or its identity, in
the key, in front of the extension that names their own. The server's verifier reads
the extension through OpenSSL (SigstoreVerifier.cpp) and never searched.

  repository_id    Fulcio's source repository identifier, 1.3.6.1.4.1.57264.1.15: an
                   extension of the tbsCertificate (its [3]) whose OCTET STRING holds
                   one DER UTF8String, the CI provider's own id of the repository
                   (GitLab's project id, GitHub's repository id). None when no
                   extension carries it. Refused (ValueError): a certificate that is
                   not one DER SEQUENCE, ends inside an element or has a byte after it;
                   an element running past the one that holds it, or a length DER
                   cannot have or would write in fewer bytes (Go's encoding/asn1, which
                   cosign reads with, refuses those too); extensions that are not one
                   SEQUENCE; an extension that
                   is not a SEQUENCE of its OID, perhaps the critical flag, and one
                   OCTET STRING; this extension twice; an OCTET STRING holding anything
                   but one UTF8String; and an id that is empty or not visible ASCII
                   (0x21 to 0x7E), which no provider's id is.
  uris             the URIs of the SubjectAltName extension (2.5.29.17), the identity
                   Fulcio names a certificate after; refused as above.
  bundle_certificate   the signing certificate of a Sigstore bundle: v0.3's certificate,
                   or the first of a v0.1 or v0.2 chain - the one cosign verifies, which
                   never looks at the rest of a chain - in strict base64. A bundle
                   holding a key twice is refused: cosign refuses one, and the server's
                   own reader (JsonDocument.cpp) takes the first where Python's takes
                   the last.

The shell reader reads a v0.3 bundle alone - its one "mediaType" must say so, and
cosign refuses a v0.3 bundle that carries a chain - and takes its one "rawBytes" (a
v0.3 bundle has one, and cosign verify-blob --new-bundle-format parses a bundle
strictly, no field twice and none it does not know). It refuses a bundle with none or
more, or with a chain, or with a raw_bytes - the name the field has in the .proto,
which cosign reads too - or with a character of ASCII written as a \\u escape, and
base64 that is not canonical. Why: cosign also reads a field's name written with \\u
escapes, and in a v0.1 or v0.2 bundle verifies the first certificate of a chain alone,
so a chain whose first certificate is spelled so that grep does not count it, and whose
second is one anyone can make, would hand a reader that counted its way to a v0.1
bundle's one "rawBytes" the second (the self-test's chain cases; cosign 2.6.5 verifies
such a bundle). The escapes refused, no certificate of the file goes uncounted, even
beside a member cosign does not know or a key twice, which cosign 2.6.5 refuses and
the shell reader does not rest on. On a v0.3 bundle as cosign writes one the two
answer the same.

Usage, from the root of a checkout:
  python3 build/ci/sigstore-certificate.py repository-id BUNDLE
        prints id=<the id>, or absent; or "refused: <why>", exiting 1. The shell
        reader, sh build/ci/sigstore-certificate.sh repository-id BUNDLE, prints the same.
  python3 build/ci/sigstore-certificate.py --self-test [--shell COMMAND]
        certificates made around the extension, and a real one Fulcio issued (6.3.3's
        installer's), each with this project's id planted in its public key: read by
        this reader, and by the shell reader under COMMAND (default sh; with od, awk,
        base64, grep, sed, wc and tr on the PATH), which must answer each as this does.
        In CI (CI or GITLAB_CI set) a missing sh fails it; elsewhere it is skipped,
        saying so. build/check-release-chain.py runs the same cases.
"""

import argparse
import base64
import binascii
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHELL_READER = os.path.join(HERE, "sigstore-certificate.sh")

# 1.3.6.1.4.1.57264.1.15 and 2.5.29.17, each as its DER OID element.
REPOSITORY_ID_OID = bytes.fromhex("060a2b0601040183bf30010f")
SUBJECT_ALT_NAME_OID = bytes.fromhex("0603551d11")
PROJECT_ID = "86730559"


# ---------------------------------------------------------------- DER

def der_length(data, at):
    """(length, where the content begins) of the DER element whose length byte is at:
    DER's alone, in as few bytes as it takes (Go's encoding/asn1, which cosign reads
    a certificate with, refuses any other)."""
    if at >= len(data):
        raise ValueError("a DER element the certificate ends inside")
    first = data[at]
    if first < 0x80:
        return first, at + 1
    count = first & 0x7F
    if count == 0 or count > 4 or at + 1 + count > len(data):
        raise ValueError("a DER length the certificate cannot have")
    length = int.from_bytes(data[at + 1:at + 1 + count], "big")
    if data[at + 1] == 0 or length < 0x80:
        raise ValueError("a length in more bytes than DER writes it in")
    return length, at + 1 + count


def der_children(data, start, end):
    """(tag, where the element begins, where its content begins, where it ends) of
    each DER element from start to end, which must hold whole elements."""
    at = start
    while at < end:
        length, content = der_length(data, at + 1)
        if content + length > end:
            raise ValueError("a DER element runs past the one that holds it")
        yield data[at], at, content, content + length
        at = content + length


def der(tag, content):
    """One DER element: the self-tests' certificates are made of these."""
    if len(content) < 0x80:
        return bytes([tag, len(content)]) + content
    size = len(content).to_bytes((len(content).bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(size)]) + size + content


# ---------------------------------------------------------------- the extensions

def extension_value(certificate, oid, what):
    """The content of the OCTET STRING of the tbsCertificate's extension `oid` (its
    DER OID element), or None when no extension is that one."""
    tag, _, start, end = next(der_children(certificate, 0, len(certificate)), (None, 0, 0, 0))
    if tag != 0x30 or end != len(certificate):
        raise ValueError("not a DER certificate")
    parts = list(der_children(certificate, start, end))
    if not parts or parts[0][0] != 0x30:
        raise ValueError("the certificate holds no tbsCertificate")
    found = None
    for tag, _, start, end in der_children(certificate, parts[0][2], parts[0][3]):
        if tag != 0xA3:  # [3] EXPLICIT Extensions
            continue
        holder = list(der_children(certificate, start, end))
        if len(holder) != 1 or holder[0][0] != 0x30:
            raise ValueError("the certificate's extensions are not a SEQUENCE")
        for extension in der_children(certificate, holder[0][2], holder[0][3]):
            if extension[0] != 0x30:
                raise ValueError("an extension that is not a SEQUENCE")
            fields = list(der_children(certificate, extension[2], extension[3]))
            if not fields or fields[0][0] != 0x06:
                raise ValueError("an extension without its OID")
            if certificate[fields[0][1]:fields[0][3]] != oid:
                continue
            if found is not None:
                raise ValueError("the %s extension is in the certificate twice" % what)
            fields = fields[1:]
            if fields and fields[0][0] == 0x01:  # critical
                fields = fields[1:]
            if len(fields) != 1 or fields[0][0] != 0x04:
                raise ValueError("the %s extension holds no OCTET STRING" % what)
            found = certificate[fields[0][2]:fields[0][3]]
    return found


def repository_id(certificate):
    """The source repository identifier Fulcio wrote into a DER certificate
    (1.3.6.1.4.1.57264.1.15, an OCTET STRING holding a UTF8String), or None.

    Read from the certificate's extensions (tbsCertificate's [3]) and nowhere else:
    the extension's bytes can be anywhere in a certificate - an RSA public key is
    bytes its holder chooses - and one who could sign at the group's path could put
    this project's id there, in front of their own."""
    value = extension_value(certificate, REPOSITORY_ID_OID, "repository id")
    if value is None:
        return None
    inner = list(der_children(value, 0, len(value)))
    if len(inner) != 1 or inner[0][0] != 0x0C:
        raise ValueError("the repository id is not a UTF8String")
    text = value[inner[0][2]:inner[0][3]]
    if not text or any(byte < 0x21 or byte > 0x7E for byte in text):
        raise ValueError("the repository id is empty or not visible ASCII, which no provider's id is")
    return text.decode("ascii")


def uris(certificate):
    """The uniformResourceIdentifier names of the certificate's SubjectAltName
    extension (the identity Fulcio names it after), in order; [] without one."""
    value = extension_value(certificate, SUBJECT_ALT_NAME_OID, "subject alternative name")
    if value is None:
        return []
    names = list(der_children(value, 0, len(value)))
    if len(names) != 1 or names[0][0] != 0x30:
        raise ValueError("the subject alternative names are not a SEQUENCE")
    found = []
    for tag, _, start, end in der_children(value, names[0][2], names[0][3]):
        if tag == 0x86:  # [6] IMPLICIT IA5String
            text = value[start:end]
            if any(byte < 0x20 or byte > 0x7E for byte in text):
                raise ValueError("a subject alternative name that is not printable ASCII")
            found.append(text.decode("ascii"))
    return found


def bundle_certificate(bundle):
    """The signing certificate, DER, of a Sigstore bundle (v0.3's certificate, or the
    first of v0.1 and v0.2's chain), its base64 read strictly. The chain only where
    there is no certificate at all: the two are one field's choices (a oneof), which
    cosign refuses together, and a certificate without its rawBytes is not one to look
    past."""
    material = bundle.get("verificationMaterial") if isinstance(bundle, dict) else None
    material = material if isinstance(material, dict) else {}
    certificate = material.get("certificate")
    chain = material.get("x509CertificateChain")
    if certificate is not None and chain is not None:
        raise ValueError("the bundle carries a certificate and a certificate chain")
    if certificate is not None:
        raw = certificate.get("rawBytes") if isinstance(certificate, dict) else None
    else:
        chain = chain.get("certificates") if isinstance(chain, dict) else None
        first = chain[0] if isinstance(chain, list) and chain else None
        raw = first.get("rawBytes") if isinstance(first, dict) else None
    if not raw or not isinstance(raw, str):
        raise ValueError("the bundle carries no certificate")
    try:
        return base64.b64decode(raw, validate=True)
    except binascii.Error:
        raise ValueError("the bundle's certificate is not base64")


def unique_keys(pairs):
    """A JSON object's members as a dict, refused when a key is there twice, or is a
    field's name as the .proto has it (raw_bytes, verification_material): cosign reads
    that name as the field as well as its JSON name (rawBytes), where this reads the
    JSON name alone, and no bundle's JSON name has an underscore. So which of two such
    names is the field is never left to cosign's refusing both (2.6.5 does)."""
    found = {}
    for key, value in pairs:
        if key in found:
            raise ValueError("the key %s is in the bundle twice" % json.dumps(key))
        if "_" in key:
            raise ValueError("the key %s is a field's name as the .proto has it, which cosign reads as its JSON name"
                             % json.dumps(key))
        found[key] = value
    return found


def read_bundle(path):
    """The signing certificate of the bundle at path; ValueError when there is none
    to read, or when a key is there twice."""
    try:
        with open(path, encoding="utf-8") as f:
            bundle = json.load(f, object_pairs_hook=unique_keys)
    except OSError as error:
        raise ValueError("%s could not be read: %s" % (path, error.strerror or error))
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise ValueError("%s is not a JSON bundle" % path)
    return bundle_certificate(bundle)


def answer(path):
    """What the command line prints for the bundle at path: id=<the id>, or absent."""
    found = repository_id(read_bundle(path))
    return "absent" if found is None else "id=" + found


# ---------------------------------------------------------------- certificates for the self-tests

def test_certificate(repository_id=None, key=b"", critical=False, twice=False, uris=(), value=None, bare=False,
                     tail=b""):
    """A certificate's shape: a tbsCertificate whose public key's bytes are `key`, and
    whose extensions carry the subject alternative names `uris` when there are any,
    and the repository id when one is given - a UTF8String, or `value`, the OCTET
    STRING's content as it is; with `bare` the extension holds its OID alone, and
    `tail` follows its OCTET STRING."""
    extensions = b""
    if uris:
        names = der(0x30, b"".join(der(0x86, u.encode("ascii")) for u in uris))
        extensions += der(0x30, SUBJECT_ALT_NAME_OID + b"\x01\x01\xff" + der(0x04, names))
    if repository_id is not None or value is not None:
        if value is None:
            value = der(0x0C, repository_id if isinstance(repository_id, bytes) else repository_id.encode("utf-8"))
        content = REPOSITORY_ID_OID + (b"\x01\x01\xff" if critical else b"") + (b"" if bare else der(0x04, value)) + tail
        extensions += der(0x30, content) * (2 if twice else 1)
    spki = der(0x30, der(0x30, der(0x06, b"\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01")) + der(0x03, b"\x00" + key))
    tbs = der(0x30, der(0xA0, der(0x02, b"\x02")) + der(0x02, b"\x01") + spki
              + (der(0xA3, der(0x30, extensions)) if extensions else b""))
    return der(0x30, tbs + der(0x30, der(0x06, b"\x2a\x86\x48\xce\x3d\x04\x03\x03")) + der(0x03, b"\x00"))


def planted(critical=False, project_id=PROJECT_ID):
    """The extension's bytes as one who wants them found would put them in a key."""
    return REPOSITORY_ID_OID + (b"\x01\x01\xff" if critical else b"") + der(0x04, der(0x0C, project_id.encode("ascii")))


def rebuilt(certificate, key=None, drop=None):
    """A certificate made again with its public key an RSA key whose modulus is `key`,
    or without the extension `drop` (its DER OID element); its signature stays what
    it was, which no reader here checks."""
    top = list(der_children(certificate, 0, len(certificate)))
    parts = list(der_children(certificate, top[0][2], top[0][3]))
    fields = [certificate[s:e] for _, s, _, e in der_children(certificate, parts[0][2], parts[0][3])]
    spki_at = 6 if fields[0][0] == 0xA0 else 5
    if key is not None:
        modulus = der(0x30, der(0x02, b"\x00" + key) + der(0x02, b"\x01\x00\x01"))
        fields[spki_at] = der(0x30, der(0x30, der(0x06, b"\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01") + b"\x05\x00")
                              + der(0x03, b"\x00" + modulus))
    if drop is not None:
        for i, field in enumerate(fields):
            if field[0] != 0xA3:
                continue
            holder = list(der_children(field, 0, len(field)))
            sequence = list(der_children(field, holder[0][2], holder[0][3]))
            kept = []
            for _, s, c, e in der_children(field, sequence[0][2], sequence[0][3]):
                oid = next(der_children(field, c, e))
                if field[oid[1]:oid[3]] != drop:
                    kept.append(field[s:e])
            fields[i] = der(0xA3, der(0x30, b"".join(kept)))
    return der(0x30, der(0x30, b"".join(fields)) + b"".join(certificate[s:e] for _, s, _, e in parts[1:]))


def body(certificate):
    """The content of a certificate's outer SEQUENCE."""
    _, content = der_length(certificate, 1)
    return certificate[content:]


def body_with(certificate, count):
    """The certificate with its outer length written in `count` bytes, a zero first."""
    content = body(certificate)
    return b"\x30" + bytes([0x80 | count]) + len(content).to_bytes(count, "big") + content


# A certificate Fulcio issued: 6.3.3's installer's, signed by the GitHub workflow the
# project had then (hMailServer-6.3.3-x64.exe.cosign.bundle in the release v6.3.3 on
# gitlab.com, SHA-256 8e5ec4cdbae1cfeb6df289bee94eef06c6e0bdac86075c70107f9167c3de7092
# as DER). Its repository id is GitHub's, 1267388815; its SAN the workflow's URI.
REAL_CERTIFICATE = base64.b64decode("".join((
    "MIIHezCCBwCgAwIBAgIUNtncn+SKzMYa7dtTJWQ6qf2wj58wCgYIKoZIzj0EAwMwNzEVMBMGA1UEChMMc2lnc3RvcmUuZGV2MR4w",
    "HAYDVQQDExVzaWdzdG9yZS1pbnRlcm1lZGlhdGUwHhcNMjYwOTE1MDEwMDA3WhcNMjYwOTE1MDExMDA3WjAAMFkwEwYHKoZIzj0C",
    "AQYIKoZIzj0DAQcDQgAEm7M/cwqKhjy0p1mhd+gKKK+Hu5bOh88YvoRiWH3j/tF9mNexK2B1kBcYqX1mfqw5Rppn5aIzek1R4nMp",
    "WjO2SqOCBh8wggYbMA4GA1UdDwEB/wQEAwIHgDATBgNVHSUEDDAKBggrBgEFBQcDAzAdBgNVHQ4EFgQUWtka/xMZlgV6LSoyOsGO",
    "50Dm23owHwYDVR0jBBgwFoAU39Ppz1YkEZb5qNjpKFWixi4YZD8wcgYDVR0RAQH/BGgwZoZkaHR0cHM6Ly9naXRodWIuY29tL1By",
    "b2dyZXNzaXZlcm9ib3QvaG1haWxzZXJ2ZXIvLmdpdGh1Yi93b3JrZmxvd3Mvc2lnbi1yZWxlYXNlLnltbEByZWZzL2hlYWRzL21h",
    "c3RlcjA5BgorBgEEAYO/MAEBBCtodHRwczovL3Rva2VuLmFjdGlvbnMuZ2l0aHVidXNlcmNvbnRlbnQuY29tMB8GCisGAQQBg78w",
    "AQIEEXdvcmtmbG93X2Rpc3BhdGNoMDYGCisGAQQBg78wAQMEKGVlNTk5NWYzNjFhMjRlZjBlNjdhZGY0ZjBjZGMxOTI5Y2VhYjE0",
    "MGUwJAYKKwYBBAGDvzABBAQWU2lnbiByZWxlYXNlIGFydGVmYWN0czAqBgorBgEEAYO/MAEFBBxQcm9ncmVzc2l2ZXJvYm90L2ht",
    "YWlsc2VydmVyMB8GCisGAQQBg78wAQYEEXJlZnMvaGVhZHMvbWFzdGVyMDsGCisGAQQBg78wAQgELQwraHR0cHM6Ly90b2tlbi5h",
    "Y3Rpb25zLmdpdGh1YnVzZXJjb250ZW50LmNvbTB0BgorBgEEAYO/MAEJBGYMZGh0dHBzOi8vZ2l0aHViLmNvbS9Qcm9ncmVzc2l2",
    "ZXJvYm90L2htYWlsc2VydmVyLy5naXRodWIvd29ya2Zsb3dzL3NpZ24tcmVsZWFzZS55bWxAcmVmcy9oZWFkcy9tYXN0ZXIwOAYK",
    "KwYBBAGDvzABCgQqDChlZTU5OTVmMzYxYTI0ZWYwZTY3YWRmNGYwY2RjMTkyOWNlYWIxNDBlMB0GCisGAQQBg78wAQsEDwwNZ2l0",
    "aHViLWhvc3RlZDA/BgorBgEEAYO/MAEMBDEML2h0dHBzOi8vZ2l0aHViLmNvbS9Qcm9ncmVzc2l2ZXJvYm90L2htYWlsc2VydmVy",
    "MDgGCisGAQQBg78wAQ0EKgwoZWU1OTk1ZjM2MWEyNGVmMGU2N2FkZjRmMGNkYzE5MjljZWFiMTQwZTAhBgorBgEEAYO/MAEOBBMM",
    "EXJlZnMvaGVhZHMvbWFzdGVyMBoGCisGAQQBg78wAQ8EDAwKMTI2NzM4ODgxNTAzBgorBgEEAYO/MAEQBCUMI2h0dHBzOi8vZ2l0",
    "aHViLmNvbS9Qcm9ncmVzc2l2ZXJvYm90MBkGCisGAQQBg78wAREECwwJMTU5ODU2ODkzMHQGCisGAQQBg78wARIEZgxkaHR0cHM6",
    "Ly9naXRodWIuY29tL1Byb2dyZXNzaXZlcm9ib3QvaG1haWxzZXJ2ZXIvLmdpdGh1Yi93b3JrZmxvd3Mvc2lnbi1yZWxlYXNlLnlt",
    "bEByZWZzL2hlYWRzL21hc3RlcjA4BgorBgEEAYO/MAETBCoMKGVlNTk5NWYzNjFhMjRlZjBlNjdhZGY0ZjBjZGMxOTI5Y2VhYjE0",
    "MGUwIQYKKwYBBAGDvzABFAQTDBF3b3JrZmxvd19kaXNwYXRjaDBjBgorBgEEAYO/MAEVBFUMU2h0dHBzOi8vZ2l0aHViLmNvbS9Q",
    "cm9ncmVzc2l2ZXJvYm90L2htYWlsc2VydmVyL2FjdGlvbnMvcnVucy8zNDkxNTQyNDM5NC9hdHRlbXB0cy8xMBYGCisGAQQBg78w",
    "ARYECAwGcHVibGljMEcGCisGAQQBg78wARgEOQw3cmVwbzpQcm9ncmVzc2l2ZXJvYm90L2htYWlsc2VydmVyOnJlZjpyZWZzL2hl",
    "YWRzL21hc3RlcjCBiQYKKwYBBAHWeQIEAgR7BHkAdwB1AN09MGrGxxEyYxkeHJlnNwKiSl643jyt/4eKcoAvKe6OAAABoKKUEPEA",
    "AAQDAEYwRAIgAfiASq4MWTDmqcm6zujVAkKgr5C2JWD/XNlerPwpDx4CIH0RRQJvqVfpOKIcPKg02aakppxldJNcRUeABcWAeabE",
    "MAoGCCqGSM49BAMDA2kAMGYCMQDCvrEAaqYDEl6lHRlCLRDoxy4gW1aQImHVS8yIDlJxE7IrcCEbEmuxJeL2miFY6aACMQCEC8vN",
    "3rLqV40GN5RmXmRaG80BtrZm8c92vxjdMUaLwHIoSpqvu94hbQD+PhUscDk=",
)))
REAL_REPOSITORY_ID = "1267388815"
REAL_URI = "https://github.com/Progressiverobot/hmailserver/.github/workflows/sign-release.yml@refs/heads/master"


def cases():
    """(what, certificate, what the command line answers: id=..., absent or refused)."""
    other = REAL_REPOSITORY_ID
    one = test_certificate(PROJECT_ID)
    padding = b"\x00" * 7
    rsa = b"\xc3" + padding + planted() + b"\x5a" * 200
    return [
        ("this project's id in the extensions", one, "id=" + PROJECT_ID),
        ("another id, in a critical extension", test_certificate(other, critical=True), "id=" + other),
        ("no repository id extension", test_certificate(uris=["https://example.org/x"]), "absent"),
        ("this project's id planted in the key, in front of the extension naming another",
         test_certificate(other, key=padding + planted()), "id=" + other),
        ("  ... planted as a critical extension", test_certificate(other, key=padding + planted(True)), "id=" + other),
        ("this project's id planted in the key, and no extension", test_certificate(key=planted()), "absent"),
        ("the extension twice", test_certificate(PROJECT_ID, twice=True), "refused"),
        ("a PrintableString, not a UTF8String", test_certificate(value=der(0x13, PROJECT_ID.encode())), "refused"),
        ("an OCTET STRING of the bare digits", test_certificate(value=PROJECT_ID.encode()), "refused"),
        ("an OCTET STRING of two UTF8Strings", test_certificate(value=der(0x0C, PROJECT_ID.encode()) + der(0x0C, b"1")),
         "refused"),
        ("a UTF8String running past its OCTET STRING", test_certificate(value=b"\x0c\x09" + PROJECT_ID.encode()), "refused"),
        ("an extension of its OID alone", test_certificate(PROJECT_ID, bare=True), "refused"),
        ("an extension with its OID alone and the critical flag", test_certificate(PROJECT_ID, bare=True, critical=True),
         "refused"),
        ("a NULL after the extension's OCTET STRING", test_certificate(PROJECT_ID, tail=der(0x05, b"")), "refused"),
        ("an id with a leading zero, which is another id", test_certificate("0" + PROJECT_ID), "id=0" + PROJECT_ID),
        ("an empty id", test_certificate(""), "refused"),
        ("an id with a space", test_certificate("8673 0559"), "refused"),
        ("an id that is not ASCII", test_certificate("\uff18\uff16\uff17\uff13\uff10\uff15\uff15\uff19"), "refused"),
        ("an id ending in a newline", test_certificate(PROJECT_ID + "\n"), "refused"),
        ("an id holding a quote", test_certificate('8673"0559'), 'id=8673"0559'),
        ("an id holding a backslash and a percent sign", test_certificate("86%73\\0559"), "id=86%73\\0559"),
        ("a certificate cut short", one[:-3], "refused"),
        ("a byte after the certificate", one + b"\x00", "refused"),
        ("an indefinite length", b"\x30\x80" + body(one) + b"\x00\x00", "refused"),
        ("a length in five bytes", b"\x30\x85" + len(body(one)).to_bytes(5, "big") + body(one), "refused"),
        ("a length in two bytes where one would do", b"\x30\x82" + len(body(one)).to_bytes(2, "big") + body(one),
         "refused"),
        ("a long length with a leading zero", body_with(REAL_CERTIFICATE, 3), "refused"),
        ("not a certificate", b"\x04\x02ab", "refused"),
        ("a certificate Fulcio issued (6.3.3's installer's)", REAL_CERTIFICATE, "id=" + REAL_REPOSITORY_ID),
        ("  ... with this project's id planted in an RSA key", rebuilt(REAL_CERTIFICATE, key=rsa), "id=" + REAL_REPOSITORY_ID),
        ("  ... planted, and its own id's extension left out",
         rebuilt(REAL_CERTIFICATE, key=rsa, drop=REPOSITORY_ID_OID), "absent"),
        ("  ... cut short", REAL_CERTIFICATE[:-1], "refused"),
    ]


def bundle_json(certificate):
    """A v0.3 bundle as cosign writes one, but for its log entry and signature."""
    return json.dumps({"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json",
                       "verificationMaterial": {"certificate": {"rawBytes": base64.b64encode(certificate).decode("ascii")},
                                                "tlogEntries": []},
                       "messageSignature": {"messageDigest": {"algorithm": "SHA2_256", "digest": "AAAA"},
                                            "signature": "MEUC"}})


def python_answer(certificate):
    try:
        found = repository_id(certificate)
    except ValueError:
        return "refused"
    return "absent" if found is None else "id=" + found


# ---------------------------------------------------------------- the two readers held to the cases

def shown(text):
    """text as ASCII, whatever a reader answered: a console need not print the rest."""
    return text.encode("unicode_escape").decode("ascii")


def shell_command(shell=None, environ=None):
    """The command the shell reader runs under, or None when there is no sh on the
    PATH of environ."""
    environ = os.environ if environ is None else environ
    if shell:
        return shlex.split(shell)
    found = shutil.which("sh", path=environ.get("PATH", os.defpath))
    return [found] if found else None


def reader_problems(shell=None, script=SHELL_READER, environ=None, answer_of=None):
    """(problems, notes): each case read by this reader (answer_of, repository_id's
    answer by default) and by the shell reader at `script` under `shell` (sh on the
    PATH when None; not at all when False), each held to what the case expects. Also
    bundles only this reader reads (a v0.1 chain) or only the shell reader refuses."""
    environ = os.environ if environ is None else environ
    answer_of = answer_of or python_answer
    problems, notes = [], []
    listed = cases()
    for what, certificate, expected in listed:
        said = answer_of(certificate)
        if said != expected:
            problems.append("sigstore-certificate.py reads %s as %s, not %s" % (what.strip(" ."), shown(said), expected))
    real_uris = uris(REAL_CERTIFICATE)
    if real_uris != [REAL_URI]:
        problems.append("sigstore-certificate.py reads the SAN of the certificate Fulcio issued as %s" % shown(repr(real_uris)))
    identity = "https://gitlab.com/hmailserver/hmailserver//.gitlab-ci.yml@refs/tags/v9.9.9"
    if uris(test_certificate(PROJECT_ID, key=identity.encode(), uris=["https://gitlab.com/someone/else"])) != [
            "https://gitlab.com/someone/else"]:
        problems.append("sigstore-certificate.py reads a SAN out of a key that holds the identity's bytes")
    if shell is False:
        notes.append("the shell reader was not asked for")
        return problems, notes
    command = shell_command(shell, environ)
    if not command:
        if environ.get("CI") or environ.get("GITLAB_CI"):
            problems.append("there is no sh to run build/ci/sigstore-certificate.sh with: container-image-deb.sh's reader "
                            "is not held to this one")
        else:
            notes.append("no sh here: build/ci/sigstore-certificate.sh was not run against the cases (CI runs it)")
        return problems, notes
    work = tempfile.mkdtemp(prefix="hm-certificate-")
    try:
        real = base64.b64encode(REAL_CERTIFICATE).decode()
        mine = base64.b64encode(test_certificate(PROJECT_ID)).decode()
        v03 = '{"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json", "verificationMaterial": %s}'
        v02 = v03.replace("v0.3", "v0.2")

        def chained(first_key):
            # A chain as cosign verifies it: the first certificate, spelled as first_key,
            # then one anyone can make, carrying this project's id.
            return v02 % ('{"x509CertificateChain": {"certificates": [{%s: "%s"}, {"rawBytes": "%s"}]}}'
                          % (first_key, real, mine))
        extra = [("a bundle whose certificate is not base64", v03 % '{"certificate": {"rawBytes": "MII$"}}',
                  "refused", "refused"),
                 ("a v0.1 bundle's chain of two certificates", json.dumps({
                     "mediaType": "application/vnd.dev.sigstore.bundle+json;version=0.1",
                     "verificationMaterial": {"x509CertificateChain": {
                         "certificates": [{"rawBytes": mine}, {"rawBytes": base64.b64encode(test_certificate()).decode()}]}}}),
                  "id=" + PROJECT_ID, "refused"),
                 ("a v0.2 bundle's chain of one certificate", v02 % ('{"x509CertificateChain": {"certificates": '
                                                                    '[{"rawBytes": "%s"}]}}' % real),
                  "id=" + REAL_REPOSITORY_ID, "refused"),
                 # cosign 2.6.5 verifies these as signed by the first certificate.
                 ("a v0.2 chain, its first certificate under raw_bytes, its second carrying this project's id",
                  chained('"raw_bytes"'), "refused", "refused"),
                 ("  ... its first under raw\\u0042ytes", chained('"raw\\u0042ytes"'), "id=" + REAL_REPOSITORY_ID, "refused"),
                 ("  ... as a v0.3 bundle", chained('"raw_bytes"').replace("v0.2", "v0.3"), "refused", "refused"),
                 ("a certificate under raw_bytes and a chain carrying this project's id",
                  v03 % ('{"certificate": {"raw_bytes": "%s"}, "x509CertificateChain": {"certificates": [{"rawBytes": "%s"}]}}'
                         % (real, mine)), "refused", "refused"),
                 ("a bundle with a second rawBytes in a field no bundle has", v03 % json.dumps({
                     "x": {"rawBytes": mine}, "certificate": {"rawBytes": real}}), "id=" + REAL_REPOSITORY_ID, "refused"),
                 ("a bundle with its certificate twice", v03 % ('{"certificate": {"rawBytes": "%s"}, "certificate": '
                                                               '{"rawBytes": "%s"}}' % (mine, real)), "refused", "refused"),
                 ("a bundle with a second mediaType in a field no bundle has",
                  v03 % ('{"certificate": {"rawBytes": "%s"}, "x": {"mediaType": "%s"}}'
                         % (real, "application/vnd.dev.sigstore.bundle.v0.3+json")), "id=" + REAL_REPOSITORY_ID, "refused"),
                 ("a bundle without a media type", '{"verificationMaterial": {"certificate": {"rawBytes": "%s"}}}' % real,
                  "id=" + REAL_REPOSITORY_ID, "refused"),
                 # The certificate cosign verified under a name written with an escape, which
                 # grep does not count, and one carrying this project's id where grep looks:
                 # cosign 2.6.5 refuses each bundle (a member it does not know, a key twice,
                 # a chain in a v0.3 bundle), and the shell reader refuses it by itself.
                 ("a v0.3 bundle, its certificate under raw\\u0042ytes, this project's id in a member no bundle has",
                  v03 % ('{"x": {"rawBytes": "%s"}, "certificate": {"raw\\u0042ytes": "%s"}}' % (mine, real)),
                  "id=" + REAL_REPOSITORY_ID, "refused"),
                 ("  ... in a second certificate under the name spelled as it is",
                  v03 % ('{"certificate": {"rawBytes": "%s"}, "\\u0063ertificate": {"raw\\u0042ytes": "%s"}}'
                         % (mine, real)), "refused", "refused"),
                 ("  ... as the second of a chain whose name is escaped",
                  v03 % ('{"x509\\u0043ertificateChain": {"certificates": [{"raw\\u0042ytes": "%s"}, {"rawBytes": "%s"}]}}'
                         % (real, mine)), "id=" + REAL_REPOSITORY_ID, "refused"),
                 ("a v0.3 bundle whose media type's name is escaped", v03.replace('"mediaType"', '"\\u006dediaType"')
                  % ('{"certificate": {"rawBytes": "%s"}}' % real), "id=" + REAL_REPOSITORY_ID, "refused"),
                 # The .proto's name of a field beside its JSON name, which cosign reads as
                 # the one field (2.6.5 refuses the two as a key twice).
                 ("this project's id under verificationMaterial, the signer's under verification_material",
                  (v03 % ('{"certificate": {"rawBytes": "%s"}}' % mine))[:-1]
                  + ', "verification_material": {"certificate": {"rawBytes": "%s"}}}' % real, "refused", "refused"),
                 ("a bundle without a certificate", v03 % '{"publicKey": {"hint": "x"}}', "refused", "refused")]
        runs = [(what, bundle_json(certificate), expected, expected) for what, certificate, expected in listed] + extra
        for number, (what, text, python_expected, shell_expected) in enumerate(runs):
            path = os.path.join(work, "bundle-%02d.json" % number)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            if number >= len(listed):
                try:
                    said = answer(path)
                except ValueError:
                    said = "refused"
                if said != python_expected:
                    problems.append("sigstore-certificate.py reads %s as %s, not %s" % (what, shown(said), python_expected))
            try:
                # Semgrep's taint rule reads the PATH as input a caller controls: here it is
                # the sh this self-test was given (--shell) or found, running this checkout's
                # reader, the arguments a list that no shell reads.
                ran = subprocess.run(command + [script, "repository-id", path],  # nosemgrep
                                     capture_output=True, timeout=60)
            except (OSError, subprocess.TimeoutExpired) as error:
                problems.append("the shell reader could not be run (%s): %s" % (" ".join(command), error))
                break
            out = ran.stdout.decode("ascii", "replace").strip("\r\n")
            said = "refused" if ran.returncode == 1 and out.startswith("refused: ") and "\n" not in out else (
                out if ran.returncode == 0 else "exit %d: %s %s" % (ran.returncode, out, ran.stderr.decode("ascii", "replace").strip()))
            if said != shell_expected:
                problems.append("sigstore-certificate.sh reads %s as %s, not %s" % (what.strip(" ."), shown(said[:200]), shell_expected))
        notes.append("%d bundles read by both readers, the shell's under %s" % (len(runs), " ".join(command)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return problems, notes


def self_test(shell=None):
    problems, notes = reader_problems(shell)
    for what, certificate, expected in cases():
        print("%s  %s: %s" % ("FAIL" if python_answer(certificate) != expected else "ok  ", what, expected))
    for note in notes:
        print("note  " + note)
    for problem in problems:
        print("FAIL  " + problem)
    print("")
    if problems:
        print("%d self-test problem(s)" % len(problems))
        return 1
    print("Every self-test case passed.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--shell", help="with --self-test: the command to run the shell reader with (default: sh)")
    commands = parser.add_subparsers(dest="command")
    read = commands.add_parser("repository-id", help="the repository id of a bundle's certificate")
    read.add_argument("bundle")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test(arguments.shell)
    if arguments.command == "repository-id":
        try:
            print(answer(arguments.bundle))
        except ValueError as error:
            print("refused: %s" % error)
            return 1
        return 0
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
