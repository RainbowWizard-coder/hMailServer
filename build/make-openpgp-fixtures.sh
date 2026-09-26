#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# Makes the OpenPGP fixtures build/check-portal-openpgp.js reads, with GnuPG
# 2.4 and nothing else, into the directory given (default build/fixtures/openpgp).
#
# THE KEYS IN THESE FIXTURES ARE TEST KEYS AND THE PASSPHRASE IS PRINTED HERE.
# They are made for this check, they belong to addresses in the .test TLD that
# can never be delivered to, and nothing on any server ever trusts them. They
# are committed so that the harnesses job needs no GnuPG; the check uses GnuPG
# as well when it finds it, which is how the other direction - what the page
# made, read by GnuPG - is proved.
#
# Run it where GnuPG 2.4 is (the lab VM), then copy the directory back:
#   sh build/make-openpgp-fixtures.sh /tmp/openpgp-fixtures
#
# It uses a scratch GNUPGHOME of its own and leaves the caller's keyring alone.
set -e

OUT="${1:-build/fixtures/openpgp}"
PASS='openpgp-fixture-pass'
SYMPASS='openpgp-fixture-symmetric'

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

GNUPGHOME=$(mktemp -d)
export GNUPGHOME
chmod 700 "$GNUPGHOME"
trap 'rm -rf "$GNUPGHOME"' EXIT

gpg --version | head -1

G="gpg --batch --yes --no-tty --pinentry-mode loopback --passphrase $PASS"

keyparams() {
   # $1 name, $2 address, $3 primary type block, $4 expire
   cat <<PARAMS
%echo making $2
$3
Name-Real: $1
Name-Email: $2
Expire-Date: $4
Passphrase: $PASS
%commit
PARAMS
}

ED_BLOCK='Key-Type: eddsa
Key-Curve: Ed25519
Key-Usage: sign,cert
Subkey-Type: ecdh
Subkey-Curve: Curve25519
Subkey-Usage: encrypt'

RSA_BLOCK='Key-Type: RSA
Key-Length: 3072
Key-Usage: sign,cert
Subkey-Type: RSA
Subkey-Length: 3072
Subkey-Usage: encrypt'

keyparams 'Alice Fixture' 'alice@fixtures.test' "$ED_BLOCK" 0 | $G --generate-key
keyparams 'Bob Fixture' 'bob@fixtures.test' "$RSA_BLOCK" 0 | $G --generate-key
keyparams 'Carol Fixture' 'carol@fixtures.test' "$ED_BLOCK" 0 | $G --generate-key
keyparams 'Dave Fixture' 'dave@fixtures.test' "$ED_BLOCK" 'seconds=3' | $G --generate-key

fpr_of() {
   gpg --batch --with-colons --list-keys "$1" | awk -F: '/^fpr:/ { print $10; exit }'
}

ALICE=$(fpr_of alice@fixtures.test)
BOB=$(fpr_of bob@fixtures.test)
CAROL=$(fpr_of carol@fixtures.test)
DAVE=$(fpr_of dave@fixtures.test)

printf '%s\n' "$ALICE" > "$OUT/alice.fpr"
printf '%s\n' "$BOB" > "$OUT/bob.fpr"
printf '%s\n' "$CAROL" > "$OUT/carol.fpr"
printf '%s\n' "$DAVE" > "$OUT/dave.fpr"

# The body every signature and every message below is made over: CRLF lines,
# one of them not ASCII, so the encodings are exercised too. part.bin is the
# same body inside the MIME entity a PGP/MIME message signs or encrypts.
python3 - "$OUT" <<'PY'
import io, os, sys
out = sys.argv[1]
body = "A message GnuPG made for the page to read.\r\nSecond line, with café and a tab:\tthere.\r\n"
io.open(os.path.join(out, "body.bin"), "wb").write(body.encode("utf-8"))
part = "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n" + body
io.open(os.path.join(out, "part.bin"), "wb").write(part.encode("utf-8"))
PY

$G --armor --export alice@fixtures.test > "$OUT/alice.pub.asc"
$G --armor --export-secret-keys alice@fixtures.test > "$OUT/alice.sec.asc"
$G --armor --export bob@fixtures.test > "$OUT/bob.pub.asc"
$G --armor --export-secret-keys bob@fixtures.test > "$OUT/bob.sec.asc"
$G --armor --export carol@fixtures.test > "$OUT/carol.pub.asc"
$G --armor --export dave@fixtures.test > "$OUT/dave.pub.asc"

# Carol is revoked: her certificate before the revocation, the revocation on
# its own, and the key with it applied - so the page's reader is checked
# against all three. GnuPG 2 writes a revocation certificate when it makes a
# key, with every line of the armour prefixed by a colon so that it cannot be
# imported by accident; the prefix comes off here.
sed 's/^://' "$GNUPGHOME/openpgp-revocs.d/$CAROL.rev" > "$OUT/carol.revocation.asc"
$G --import "$OUT/carol.revocation.asc"
$G --armor --export carol@fixtures.test > "$OUT/carol.revoked.pub.asc"

# Dave's key was made to expire three seconds after it was made.
sleep 4

$G --armor --local-user alice@fixtures.test --detach-sign --output "$OUT/detached-ed25519.sig" "$OUT/body.bin"
$G --armor --local-user bob@fixtures.test --detach-sign --output "$OUT/detached-rsa.sig" "$OUT/body.bin"
$G --armor --local-user alice@fixtures.test --detach-sign --output "$OUT/detached-part.sig" "$OUT/part.bin"
$G --armor --local-user alice@fixtures.test --clearsign --output "$OUT/cleartext-ed25519.asc" "$OUT/body.bin"
$G --armor --trust-model always --local-user bob@fixtures.test --sign --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-part-to-alice.asc" "$OUT/part.bin"

$G --armor --trust-model always --local-user bob@fixtures.test --sign --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-to-alice-signed.asc" "$OUT/body.bin"
$G --armor --trust-model always --local-user alice@fixtures.test --sign --encrypt \
   --recipient bob@fixtures.test --output "$OUT/encrypted-to-bob-signed.asc" "$OUT/body.bin"
$G --armor --trust-model always -z 0 --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-to-alice-plain.asc" "$OUT/body.bin"
$G --armor --trust-model always -z 6 --compress-algo zlib --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-to-alice-zlib.asc" "$OUT/body.bin"
$G --armor --trust-model always -z 6 --compress-algo zip --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-to-alice-zip.asc" "$OUT/body.bin"
$G --armor --trust-model always -z 0 --cipher-algo AES128 --encrypt \
   --recipient alice@fixtures.test --output "$OUT/encrypted-to-alice-aes128.asc" "$OUT/body.bin"
$G --armor --trust-model always -z 0 --encrypt \
   --recipient alice@fixtures.test --recipient bob@fixtures.test \
   --output "$OUT/encrypted-to-both.asc" "$OUT/body.bin"

gpg --batch --yes --no-tty --pinentry-mode loopback --passphrase "$SYMPASS" \
    --armor -z 0 --symmetric --output "$OUT/symmetric.asc" "$OUT/body.bin"

# The two PGP/MIME messages, assembled from what GnuPG made: the shapes RFC
# 3156 names, which is what Thunderbird and every other client sends.
python3 - "$OUT" <<'PY'
import io, os, sys
out = sys.argv[1]

def read(name):
    return io.open(os.path.join(out, name), "rb").read()

part = read("part.bin")
signature = read("detached-part.sig")
encrypted = read("encrypted-part-to-alice.asc")

boundary = b"=_fixture_signed_0001"
signed = (b"From: Alice Fixture <alice@fixtures.test>\r\n"
          b"To: Bob Fixture <bob@fixtures.test>\r\n"
          b"Subject: A signed fixture\r\n"
          b"MIME-Version: 1.0\r\n"
          b"Content-Type: multipart/signed; micalg=pgp-sha512;\r\n"
          b" protocol=\"application/pgp-signature\"; boundary=\"" + boundary + b"\"\r\n\r\n"
          b"This is an OpenPGP/MIME signed message (RFC 3156).\r\n\r\n"
          b"--" + boundary + b"\r\n" + part +
          b"\r\n--" + boundary + b"\r\n"
          b"Content-Type: application/pgp-signature; name=\"signature.asc\"\r\n"
          b"Content-Description: OpenPGP digital signature\r\n\r\n" + signature +
          b"--" + boundary + b"--\r\n")
io.open(os.path.join(out, "pgpmime-signed.eml"), "wb").write(signed)

boundary = b"=_fixture_encrypted_0001"
message = (b"From: Bob Fixture <bob@fixtures.test>\r\n"
           b"To: Alice Fixture <alice@fixtures.test>\r\n"
           b"Subject: An encrypted fixture\r\n"
           b"MIME-Version: 1.0\r\n"
           b"Content-Type: multipart/encrypted;\r\n"
           b" protocol=\"application/pgp-encrypted\"; boundary=\"" + boundary + b"\"\r\n\r\n"
           b"This is an OpenPGP/MIME encrypted message (RFC 3156).\r\n\r\n"
           b"--" + boundary + b"\r\n"
           b"Content-Type: application/pgp-encrypted\r\n"
           b"Content-Description: PGP/MIME version identification\r\n\r\n"
           b"Version: 1\r\n\r\n"
           b"--" + boundary + b"\r\n"
           b"Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\n"
           b"Content-Description: OpenPGP encrypted message\r\n\r\n" + encrypted +
           b"--" + boundary + b"--\r\n")
io.open(os.path.join(out, "pgpmime-encrypted.eml"), "wb").write(message)
PY

cat > "$OUT/README.md" <<'READ'
# OpenPGP fixtures

Everything here was made by `build/make-openpgp-fixtures.sh` with GnuPG 2.4 and
is read by `build/check-portal-openpgp.js`, which is how the webmail's OpenPGP
module is checked against an implementation that is not this one.

**The secret keys here are test keys and their passphrase is
`openpgp-fixture-pass`** (the symmetric message's is
`openpgp-fixture-symmetric`). They were made for this check, their addresses
are in the `.test` top-level domain, which can never be delivered to, and
nothing anywhere trusts them. They are committed so that the check (the
harnesses job of .gitlab-ci.yml) needs no GnuPG; where the check finds `gpg` on the PATH it also runs the other
direction - what the module made, handed to GnuPG - which is the half that
cannot be proved from a file.

Never copy a key from here into anything real.
READ

ls -la "$OUT"
echo "fixtures written to $OUT"
echo "server-sealed-to-alice.eml is not made here: it is the server's writer's, see README.md"
