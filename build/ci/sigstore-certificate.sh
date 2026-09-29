#!/bin/sh
# https://www.progressiverobot.com
# Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
# SPDX-License-Identifier: AGPL-3.0-or-later

# The repository id in a Sigstore bundle's certificate, where there is no Python:
# build/ci/container-image-deb.sh, in cosign's own image (HM_COSIGN_IMAGE), whose
# busybox has sh, od, awk and base64 and nothing that reads a certificate - no
# openssl, and cosign verify-blob checks no CI's extensions but GitHub's. The reading
# is build/ci/sigstore-certificate.py's repository_id, which holds this one to itself
# over every case of its self-test (build/check-release-chain.py runs them):
#
#   sh build/ci/sigstore-certificate.sh repository-id BUNDLE
#
# prints id=<the id> when the certificate's extensions carry Fulcio's source
# repository identifier (1.3.6.1.4.1.57264.1.15), absent when they do not, and
# otherwise "refused: <why>", exiting 1. The id is read from the extension - the
# tbsCertificate's [3], each Extension's OID, critical flag and OCTET STRING, which
# must hold one UTF8String - and from nowhere else: a certificate's public key is
# bytes its holder chooses, and could hold this project's id in front of the
# extension that names another. Refused as the Python reader refuses: a certificate
# that is not one DER SEQUENCE or ends inside an element; an element that runs past
# the one holding it, or a length DER would write shorter; the extension twice; an
# extension without its OCTET STRING, or with more; anything but one UTF8String in
# it; and an id that is empty or not visible ASCII.
#
# The certificate is the bundle's one "rawBytes": verificationMaterial.certificate's
# in a v0.3 bundle, which cosign verify-blob --new-bundle-format reads strictly (no
# field twice, none it does not know). A bundle with none, or with more - a v0.1
# chain - is refused, as is base64 that is not canonical.
#
# And only a v0.3 bundle, by its one "mediaType": cosign also reads the name a field
# has in the .proto (raw_bytes) and a name written with \u escapes, and in a v0.1 or
# v0.2 bundle verifies the first certificate of a chain and never looks at the rest -
# so a chain whose first certificate, the one cosign verified, is spelled so that no
# grep counts it, and whose second is a certificate anyone can make, would hand this
# reader the second. A v0.3 bundle cosign refuses to carry a chain ("verification
# material cannot be X.509 certificate chain"), and holds one certificate, whose
# rawBytes, spelled as grep finds it, is then the one "rawBytes" there is. The media
# type is the one key of that name a bundle has, and cosign refuses it twice in any
# spelling, so the one literal "mediaType" is the bundle's own. And a bundle that
# writes any character of ASCII as a \u escape is refused, the one way a name is
# spelled so that grep does not find it (a .proto name aside, refused by name): so no
# certificate is left uncounted even where cosign would take what 2.6.5 refuses.
set -eu
LC_ALL=C
export LC_ALL

refuse() {
  echo "refused: $*"
  exit 1
}

if [ "$#" -ne 2 ] || [ "$1" != repository-id ]; then
  echo "usage: sh build/ci/sigstore-certificate.sh repository-id BUNDLE" >&2
  exit 2
fi
bundle=$2
[ -f "$bundle" ] || refuse "there is no $bundle"

count=$(grep -o '"mediaType"' "$bundle" | wc -l | tr -d ' ')
media=$(grep -o '"mediaType" *: *"[^"]*"' "$bundle" | sed 's/.*"\([^"]*\)"$/\1/')
case "$count:$media" in
  "1:application/vnd.dev.sigstore.bundle.v0.3+json" | "1:application/vnd.dev.sigstore.bundle+json;version=0.3") ;;
  *) refuse "the bundle is not one v0.3 Sigstore bundle, whose one certificate is the one cosign verifies" ;;
esac
if grep -q -e '"x509CertificateChain"' -e '"x509_certificate_chain"' -e '"raw_bytes"' "$bundle"; then
  refuse "the bundle holds a certificate chain, or a certificate's bytes under a name this reader does not count"
fi
# Nor a character of ASCII written as a \u escape: no encoder writes one (cosign's
# bundles have none), and a name so written - "raw\u0042ytes",
# "x509\u0043ertificateChain" - is one cosign reads and grep does not count.
# Refused, the one literal "rawBytes" is the one certificate the file has whatever
# cosign would make of the rest (a member it does not know, a key twice, a chain in a
# v0.3 bundle), not only because cosign 2.6.5 refuses each of those.
if grep -q '\\u00[2-7][0-9A-Fa-f]' "$bundle"; then
  refuse "the bundle writes a character of ASCII as an escape, which could hide a name from this reader"
fi
count=$(grep -o '"rawBytes"' "$bundle" | wc -l | tr -d ' ')
[ "$count" = 1 ] || refuse "the bundle holds $count rawBytes, not the one certificate of a v0.3 bundle"
raw=$(grep -o '"rawBytes" *: *"[^"]*"' "$bundle" | sed 's/.*"\([^"]*\)"$/\1/')
case "$raw" in
  "" | *[!A-Za-z0-9+/=]* | *=*[!=]* | *===*) refuse "the bundle's certificate is not base64" ;;
esac
length=${#raw}
[ $((length % 4)) -eq 0 ] || refuse "the bundle's certificate is not base64"
pad=0
case "$raw" in
  *==) pad=2 ;;
  *=) pad=1 ;;
esac
# What base64 must decode to, so that a decoder stopping short is caught too.
size=$((length * 3 / 4 - pad))

# The certificate as decimal bytes, one number a field, walked as DER.
# shellcheck disable=SC2016
program='
function refuse(why) {
  print "refused: " why
  exit 1
}
# LEN and CONTENT of the element whose length byte is at at.
function length_at(at,    first, count, k, v) {
  if (at >= n) refuse("a DER element the certificate ends inside")
  first = b[at]
  if (first < 128) {
    LEN = first
    CONTENT = at + 1
    return
  }
  count = first - 128
  if (count == 0 || count > 4 || at + 1 + count > n) refuse("a DER length the certificate cannot have")
  v = 0
  for (k = 1; k <= count; k++) v = v * 256 + b[at + k]
  if (b[at + 1] == 0 || v < 128) refuse("a length in more bytes than DER writes it in")
  LEN = v
  CONTENT = at + 1 + count
}
# The elements from start to end, which must hold whole ones, as TAG[key, i],
# FROM[key, i], BODY[key, i] (where the content begins) and TO[key, i]; answers how many.
function children(start, end, key,    at, m) {
  m = 0
  at = start
  while (at < end) {
    length_at(at + 1)
    if (CONTENT + LEN > end) refuse("a DER element runs past the one that holds it")
    TAG[key, m] = b[at]
    FROM[key, m] = at
    BODY[key, m] = CONTENT
    TO[key, m] = CONTENT + LEN
    m++
    at = CONTENT + LEN
  }
  return m
}
function is_oid(from, to,    k) {
  if (to - from != 12) return 0
  for (k = 0; k < 12; k++) if (b[from + k] != OID[k]) return 0
  return 1
}
BEGIN {
  # 1.3.6.1.4.1.57264.1.15 as its DER OID element: 06 0a 2b 06 01 04 01 83 bf 30 01 0f.
  split("6 10 43 6 1 4 1 131 191 48 1 15", o, " ")
  for (k = 1; k <= 12; k++) OID[k - 1] = o[k] + 0
  n = 0
}
{
  for (i = 1; i <= NF; i++) b[n++] = $i + 0
}
END {
  if (n != size) refuse("the certificate did not decode whole")
  length_at(1)
  if (b[0] != 48 || CONTENT + LEN != n) refuse("not a DER certificate")
  if (children(CONTENT, n, "c") < 1 || TAG["c", 0] != 48) refuse("the certificate holds no tbsCertificate")
  found = 0
  tbs = children(BODY["c", 0], TO["c", 0], "t")
  for (i = 0; i < tbs; i++) {
    if (TAG["t", i] != 163) continue
    if (children(BODY["t", i], TO["t", i], "h") != 1 || TAG["h", 0] != 48) refuse("the extensions of the certificate are not a SEQUENCE")
    extensions = children(BODY["h", 0], TO["h", 0], "e")
    for (j = 0; j < extensions; j++) {
      if (TAG["e", j] != 48) refuse("an extension that is not a SEQUENCE")
      fields = children(BODY["e", j], TO["e", j], "f")
      if (fields < 1 || TAG["f", 0] != 6) refuse("an extension without its OID")
      if (!is_oid(FROM["f", 0], TO["f", 0])) continue
      if (found) refuse("the repository id extension is in the certificate twice")
      k = 1
      if (k < fields && TAG["f", k] == 1) k++
      if (fields - k != 1 || TAG["f", k] != 4) refuse("the repository id extension holds no OCTET STRING")
      if (children(BODY["f", k], TO["f", k], "i") != 1 || TAG["i", 0] != 12) refuse("the repository id is not a UTF8String")
      if (BODY["i", 0] == TO["i", 0]) refuse("the repository id is empty or not visible ASCII, which the id of no provider is")
      value = ""
      for (p = BODY["i", 0]; p < TO["i", 0]; p++) {
        if (b[p] < 33 || b[p] > 126) refuse("the repository id is empty or not visible ASCII, which the id of no provider is")
        value = value sprintf("%c", b[p])
      }
      found = 1
    }
  }
  if (found) print "id=" value
  else print "absent"
}'
printf '%s' "$raw" | base64 -d 2> /dev/null | od -An -v -tu1 | awk -v size="$size" "$program"
