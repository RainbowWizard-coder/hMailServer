// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// OpenPGP for the webmail page, in the browser, on the Web Crypto API, BigInt
// and one small AES of its own: RFC 4880 version 4 keys and signatures as
// GnuPG 2.4 emits them by default, the ASCII armour around them, PGP/MIME
// (RFC 3156) on either side, and Autocrypt (Level 1) headers.
//
// Served as /portal-openpgp.js and loaded by the page only when a message or a
// setting needs it; also loadable in Node (module.exports), which is how
// build/check-portal-openpgp.js runs it against artefacts GnuPG 2.4 made and
// hands what it made back to GnuPG.
//
// THE MIME LAYER IS THE S/MIME MODULE'S. Parsing an entity, decoding a body,
// pulling the text, the HTML and the attachments out of one, building one and
// writing RFC 5322 headers over it are all done exactly once in this page, in
// PortalSmime.js, and this module calls them (window.hmSmime in the page, which
// the shell always loads first; require('./PortalSmime.js') in Node). Two
// implementations of MIME in one page would be two sets of edge cases.
//
// WHERE THE WEB CRYPTO API STOPS
//
//   * AES-CFB, which is what an OpenPGP message body is encrypted with, is not
//     in Web Crypto. CFB needs the block cipher in the ENCRYPT direction for
//     both directions, and each block's input is the previous block's
//     ciphertext, so it cannot be batched into one AES-CTR call: it would be
//     one asynchronous call per sixteen bytes, which for a one-megabyte
//     message is sixty-five thousand of them. So the block cipher is here, in
//     about a hundred lines - key schedule and encrypt-block, no inverse
//     cipher, because CFB never needs one. It is a table-free byte-at-a-time
//     implementation and therefore NOT constant time; said plainly, because
//     the alternative would be to say nothing. It runs in the reader's own
//     browser on the reader's own mail, where the attacker who could measure
//     its cache timing is already running code in that browser.
//   * The AES key wrap of RFC 3394, which RFC 6637 uses to wrap a session key
//     under the key the ECDH agreement derived, IS in Web Crypto ("AES-KW"),
//     and is used from there rather than written again - the payload is
//     carried in and out as a raw HMAC key, which is the one way Web Crypto
//     lets arbitrary octets through wrapKey and unwrapKey.
//   * RSAES-PKCS1-v1_5, the key transport OpenPGP uses for an RSA subkey, is
//     not in Web Crypto: the session key is encrypted with the public exponent
//     and decrypted with the private key's CRT parameters, in BigInt, exactly
//     as PortalSmime.js does it for S/MIME.
//   * RSASSA-PKCS1-v1_5 signatures, SHA-1 through SHA-512, PBKDF2 and the
//     random numbers all go through Web Crypto.
//   * Ed25519 and X25519 go through Web Crypto where the browser has them.
//     Chrome 137, Firefox 130, Safari 17 and Node 18 do; an older browser is
//     told so by name (curve25519Available()), and RSA still works there.
//
// WHAT IS IMPLEMENTED
//
//   Keys: version 4 public and secret keys, RSA (2048/3072/4096) and
//   Curve25519 (Ed25519 signing - the legacy EdDSA algorithm 22 with the
//   Ed25519Legacy curve, which is what GnuPG writes - and X25519 key agreement
//   as ECDH algorithm 18 with the cv25519 curve, the RFC 6637 KDF and the AES
//   key wrap). Secret key material at rest is encrypted the OpenPGP way -
//   iterated-and-salted S2K, AES-256 in CFB, the SHA-1 checksum of usage octet
//   254 - so a key this page exports opens in GnuPG, and a key GnuPG exports
//   opens here.
//
//   Packets: public-key (6), public-subkey (14), secret-key (5),
//   secret-subkey (7), signature (2), one-pass signature (4), user id (13),
//   user attribute (17, kept and skipped), literal data (11), compressed data
//   (8: ZIP and ZLIB read through DecompressionStream, nothing written
//   compressed), symmetrically encrypted and integrity protected data (18,
//   version 1, AES-CFB with the SHA-1 modification detection code),
//   symmetrically encrypted data (9, read only, with the CFB resynchronisation
//   that packet alone uses), public-key encrypted session key (1) and
//   symmetric-key encrypted session key (3).
//
//   Messages: PGP/MIME multipart/signed with application/pgp-signature and
//   multipart/encrypted with application/pgp-encrypted - sign, encrypt, sign
//   and encrypt, verify and decrypt - and inline PGP read only: a
//   "BEGIN PGP MESSAGE" or a cleartext-signed block in a text part is opened,
//   but nothing is ever written inline.
//
//   BLAKE2b (RFC 7693) and Argon2d, Argon2i and Argon2id (RFC 9106), for the
//   one thing that needs a memory-hard derivation and that Web Crypto has
//   none for: the passphrase a zero-access mailbox's key is sealed under,
//   derived from the account password (docs/ZeroAccess.md).
//
// WHAT IS NOT
//
//   Version 5 and version 6 keys and signatures, AEAD (the OCB and EAX modes
//   of RFC 9580 and of the librepgp draft), smartcards, DSA and ElGamal keys,
//   the NIST and Brainpool curves, 3DES, CAST5, IDEA, Blowfish, Twofish and
//   Camellia, BZip2, writing anything compressed, writing inline PGP, and the
//   web of trust - a key is the one the reader imported, Autocrypt learned or
//   the directory answered, and nothing here signs anybody else's key. Each of
//   those is refused by name where it is met, never silently mis-read.
//   hmailserver/docs/OpenPGP.md says the same list in prose.
(function (root, factory) {
  if (typeof module === 'object' && module.exports) { module.exports = factory(function () { return require('./PortalSmime.js'); }); }
  else { root.hmOpenPgp = factory(function () { return root.hmSmime; }); }
}(typeof self !== 'undefined' ? self : this, function (mimeModule) {
  'use strict';

  const cryptoApi = (typeof crypto !== 'undefined' && crypto.subtle) ? crypto : (typeof globalThis !== 'undefined' ? globalThis.crypto : null);
  const subtle = cryptoApi && cryptoApi.subtle;
  const textEncoder = new TextEncoder();

  // The S/MIME module, asked for when it is needed rather than when this file
  // is evaluated: in the page the two scripts are separate <script> elements
  // and this one may be fetched first.
  function mime() {
    const found = mimeModule();
    if (!found) { throw new Error('the page\'s MIME module (portal-smime.js) did not load'); }
    return found;
  }

  // ---- bytes ---------------------------------------------------------------

  function utf8(text) { return textEncoder.encode(text); }

  function concat(list) {
    let total = 0;
    for (const part of list) { total += part.length; }
    const out = new Uint8Array(total);
    let at = 0;
    for (const part of list) { out.set(part, at); at += part.length; }
    return out;
  }

  function equalBytes(a, b) {
    if (!a || !b || a.length !== b.length) { return false; }
    let diff = 0;
    for (let i = 0; i < a.length; i++) { diff |= a[i] ^ b[i]; }
    return diff === 0;
  }

  function hex(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i++) { s += (bytes[i] < 16 ? '0' : '') + bytes[i].toString(16); }
    return s;
  }

  function fromHex(text) {
    const clean = String(text).replace(/[^0-9a-fA-F]/g, '');
    const out = new Uint8Array(clean.length >> 1);
    for (let i = 0; i < out.length; i++) { out[i] = parseInt(clean.substr(i * 2, 2), 16); }
    return out;
  }

  function binaryString(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) { s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000)); }
    return s;
  }

  function latin1Bytes(text) {
    const out = new Uint8Array(text.length);
    for (let i = 0; i < text.length; i++) { out[i] = text.charCodeAt(i) & 0xff; }
    return out;
  }

  function b64encode(bytes) { return btoa(binaryString(bytes)); }

  function b64decode(text) {
    const clean = String(text).replace(/[^A-Za-z0-9+/=]/g, '');
    const s = atob(clean);
    const out = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) { out[i] = s.charCodeAt(i); }
    return out;
  }

  function b64url(bytes) { return b64encode(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); }
  function b64urlDecode(text) { return b64decode(String(text).replace(/-/g, '+').replace(/_/g, '/')); }

  function randomBytes(length) { const out = new Uint8Array(length); cryptoApi.getRandomValues(out); return out; }

  function uint16(value) { return new Uint8Array([(value >> 8) & 0xff, value & 0xff]); }
  function uint32(value) { return new Uint8Array([(value >>> 24) & 0xff, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff]); }
  function readUint16(bytes, at) { return (bytes[at] << 8) | bytes[at + 1]; }
  function readUint32(bytes, at) { return ((bytes[at] << 24) >>> 0) + (bytes[at + 1] << 16) + (bytes[at + 2] << 8) + bytes[at + 3]; }

  // ---- big integers --------------------------------------------------------

  function bytesToBig(bytes) { return bytes.length ? BigInt('0x' + hex(bytes)) : 0n; }

  function bigToBytes(value, length) {
    let h = value.toString(16);
    if (h.length % 2) { h = '0' + h; }
    const raw = fromHex(h);
    if (length === undefined) { return raw; }
    if (raw.length > length) { throw new Error('integer too large'); }
    const out = new Uint8Array(length);
    out.set(raw, length - raw.length);
    return out;
  }

  function modPow(base, exponent, modulus) {
    let result = 1n;
    base %= modulus;
    while (exponent > 0n) {
      if (exponent & 1n) { result = (result * base) % modulus; }
      exponent >>= 1n;
      base = (base * base) % modulus;
    }
    return result;
  }

  // The inverse of a modulo m, by the extended Euclidean algorithm.
  function modInverse(a, m) {
    let [old_r, r] = [((a % m) + m) % m, m];
    let [old_s, s] = [1n, 0n];
    while (r !== 0n) {
      const q = old_r / r;
      [old_r, r] = [r, old_r - q * r];
      [old_s, s] = [s, old_s - q * s];
    }
    if (old_r !== 1n) { throw new Error('the key\'s numbers are not coprime'); }
    return ((old_s % m) + m) % m;
  }

  // ---- ASCII armour (RFC 4880 section 6) -----------------------------------
  //
  // The radix-64 block with its header line, its optional comment headers, its
  // CRC-24 checksum line and its tail. A block whose checksum does not match
  // is refused: a key or a message that lost a byte in a mail client's
  // line-wrapping is a key that will fail later, in a less obvious place.

  function crc24(bytes) {
    let crc = 0xB704CE;
    for (let i = 0; i < bytes.length; i++) {
      crc ^= bytes[i] << 16;
      for (let bit = 0; bit < 8; bit++) {
        crc <<= 1;
        if (crc & 0x1000000) { crc ^= 0x1864CFB; }
      }
    }
    return crc & 0xFFFFFF;
  }

  function lines64(text) {
    let out = '';
    for (let i = 0; i < text.length; i += 64) { out += text.slice(i, i + 64) + '\r\n'; }
    return out;
  }

  // type is what goes after "PGP " on the header line: "PUBLIC KEY BLOCK",
  // "PRIVATE KEY BLOCK", "MESSAGE", "SIGNATURE" or "PUBLIC KEY BLOCK".
  function armor(type, bytes, headers) {
    const sum = crc24(bytes);
    let out = '-----BEGIN PGP ' + type + '-----\r\n';
    const named = headers || {};
    for (const name in named) { if (named[name]) { out += name + ': ' + named[name] + '\r\n'; } }
    out += '\r\n' + lines64(b64encode(bytes));
    out += '=' + b64encode(new Uint8Array([(sum >> 16) & 0xff, (sum >> 8) & 0xff, sum & 0xff])) + '\r\n';
    out += '-----END PGP ' + type + '-----\r\n';
    return out;
  }

  function dearmor(text) {
    const source = String(text).replace(/\r\n/g, '\n');
    const begin = source.match(/-----BEGIN PGP ([A-Z0-9, ]+)-----/);
    if (!begin) { throw new Error('not an OpenPGP armoured block'); }
    const type = begin[1].trim();
    const after = source.slice(begin.index + begin[0].length);
    const end = after.indexOf('-----END PGP ');
    if (end < 0) { throw new Error('the armoured block has no end line'); }
    const body = after.slice(0, end);
    // The headers run to the first blank line; what follows is the radix-64.
    const blank = body.indexOf('\n\n');
    const headerText = blank < 0 ? '' : body.slice(0, blank);
    const dataText = blank < 0 ? body : body.slice(blank + 2);
    const headers = {};
    headerText.split('\n').forEach(function (line) {
      const colon = line.indexOf(':');
      if (colon > 0) { headers[line.slice(0, colon).trim()] = line.slice(colon + 1).trim(); }
    });
    let checksum = null;
    const data = [];
    dataText.split('\n').forEach(function (line) {
      const clean = line.trim();
      if (!clean) { return; }
      if (clean[0] === '=') { checksum = clean.slice(1); return; }
      data.push(clean);
    });
    const bytes = b64decode(data.join(''));
    if (checksum) {
      const given = b64decode(checksum);
      const sum = crc24(bytes);
      if (given.length !== 3 || given[0] !== ((sum >> 16) & 0xff) || given[1] !== ((sum >> 8) & 0xff) || given[2] !== (sum & 0xff)) {
        throw new Error('the armoured block\'s checksum does not match its contents');
      }
    }
    return { type: type, headers: headers, bytes: bytes };
  }

  // Binary bytes, or an armoured block, whichever arrived.
  function anyToBytes(input) {
    if (input instanceof Uint8Array) {
      if (input.length > 10 && binaryString(input.subarray(0, 11)) === '-----BEGIN ') { return dearmor(binaryString(input)).bytes; }
      return input;
    }
    const text = String(input);
    if (text.indexOf('-----BEGIN PGP') >= 0) { return dearmor(text).bytes; }
    return b64decode(text);
  }

  // ---- packets (RFC 4880 section 4) ----------------------------------------

  const TAG = {
    PKESK: 1, SIGNATURE: 2, SKESK: 3, ONE_PASS: 4, SECRET_KEY: 5, PUBLIC_KEY: 6,
    SECRET_SUBKEY: 7, COMPRESSED: 8, SED: 9, MARKER: 10, LITERAL: 11, TRUST: 12,
    USER_ID: 13, PUBLIC_SUBKEY: 14, USER_ATTRIBUTE: 17, SEIPD: 18, MDC: 19
  };

  function writeLength(length) {
    if (length < 192) { return new Uint8Array([length]); }
    if (length < 8384) { const l = length - 192; return new Uint8Array([(l >> 8) + 192, l & 0xff]); }
    return concat([new Uint8Array([0xff]), uint32(length)]);
  }

  function writePacket(tag, body) {
    return concat([new Uint8Array([0xc0 | tag]), writeLength(body.length), body]);
  }

  // Every packet in the stream, both header formats, and the partial body
  // lengths GnuPG writes whenever it does not know how long a packet will be
  // before it writes it - which is every message it pipes rather than reads
  // from a file. A partial packet's body is the chunks joined; the caller sees
  // one body either way.
  function readPackets(bytes) {
    const packets = [];
    let at = 0;
    while (at < bytes.length) {
      const header = bytes[at];
      if (!(header & 0x80)) { throw new Error('a packet header at byte ' + at + ' does not begin with its tag bit'); }
      let tag;
      let length;
      let partial = false;
      at++;
      if (header & 0x40) {
        tag = header & 0x3f;
        const first = bytes[at++];
        if (first < 192) { length = first; }
        else if (first < 224) { length = ((first - 192) << 8) + bytes[at++] + 192; }
        else if (first === 255) { length = readUint32(bytes, at); at += 4; }
        else { length = 1 << (first & 0x1f); partial = true; }
      }
      else {
        tag = (header >> 2) & 0x0f;
        const kind = header & 0x03;
        if (kind === 0) { length = bytes[at++]; }
        else if (kind === 1) { length = readUint16(bytes, at); at += 2; }
        else if (kind === 2) { length = readUint32(bytes, at); at += 4; }
        else { length = bytes.length - at; }
      }
      if (!partial) {
        if (at + length > bytes.length) { throw new Error('a packet says it is ' + length + ' bytes and only ' + (bytes.length - at) + ' remain'); }
        packets.push({ tag: tag, body: bytes.subarray(at, at + length) });
        at += length;
        continue;
      }
      const chunks = [];
      for (;;) {
        if (at + length > bytes.length) { throw new Error('a partial packet says it is ' + length + ' bytes and only ' + (bytes.length - at) + ' remain'); }
        chunks.push(bytes.subarray(at, at + length));
        at += length;
        const next = bytes[at++];
        if (next < 192) { length = next; break; }
        if (next < 224) { length = ((next - 192) << 8) + bytes[at++] + 192; break; }
        if (next === 255) { length = readUint32(bytes, at); at += 4; break; }
        length = 1 << (next & 0x1f);
      }
      if (at + length > bytes.length) { throw new Error('a partial packet\'s last chunk says it is ' + length + ' bytes and only ' + (bytes.length - at) + ' remain'); }
      chunks.push(bytes.subarray(at, at + length));
      at += length;
      packets.push({ tag: tag, body: concat(chunks) });
    }
    return packets;
  }

  // ---- multiprecision integers ---------------------------------------------

  function readMpi(bytes, at) {
    const bits = readUint16(bytes, at);
    const size = (bits + 7) >> 3;
    if (at + 2 + size > bytes.length) { throw new Error('a number in the key runs past the end of its packet'); }
    return { bits: bits, bytes: bytes.subarray(at + 2, at + 2 + size), next: at + 2 + size };
  }

  function writeMpi(value) {
    // The bit length of the value, with its leading zero octets dropped, which
    // is what every other implementation writes and what a fingerprint is
    // computed over.
    let start = 0;
    while (start < value.length && value[start] === 0) { start++; }
    const trimmed = value.subarray(start);
    if (!trimmed.length) { return new Uint8Array([0, 0]); }
    let bits = (trimmed.length - 1) * 8;
    let top = trimmed[0];
    while (top) { bits++; top >>= 1; }
    return concat([uint16(bits), trimmed]);
  }

  // ---- hashes --------------------------------------------------------------

  const HASH = { SHA1: 2, SHA256: 8, SHA384: 9, SHA512: 10 };

  function hashName(id) {
    if (id === HASH.SHA1) { return 'SHA-1'; }
    if (id === HASH.SHA256) { return 'SHA-256'; }
    if (id === HASH.SHA384) { return 'SHA-384'; }
    if (id === HASH.SHA512) { return 'SHA-512'; }
    if (id === 1) { throw new Error('MD5 signatures are not read by this page'); }
    if (id === 3) { throw new Error('RIPEMD-160 signatures are not read by this page'); }
    if (id === 11) { throw new Error('SHA-224 signatures are not read by this page: the browser has no SHA-224'); }
    throw new Error('hash algorithm ' + id + ' is not one this page knows');
  }

  async function digest(id, bytes) { return new Uint8Array(await subtle.digest(hashName(id), bytes)); }
  async function sha1(bytes) { return new Uint8Array(await subtle.digest('SHA-1', bytes)); }

  // ---- AES: the block cipher, and CFB over it ------------------------------
  //
  // Encrypt only: CFB uses the cipher in the encrypt direction for both
  // encrypting and decrypting, and the one place an inverse cipher would be
  // needed - the AES key wrap of RFC 3394 - goes through Web Crypto instead.

  const AES_SBOX = (function () {
    const sbox = new Uint8Array(256);
    let p = 1;
    let q = 1;
    do {
      // p *= 3 in GF(2^8)
      p = p ^ ((p << 1) & 0xff) ^ ((p & 0x80) ? 0x1b : 0);
      // q /= 3 in GF(2^8)
      q ^= (q << 1) & 0xff;
      q ^= (q << 2) & 0xff;
      q ^= (q << 4) & 0xff;
      if (q & 0x80) { q ^= 0x09; }
      const value = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4));
      sbox[p] = (value ^ 0x63) & 0xff;
    } while (p !== 1);
    sbox[0] = 0x63;
    return sbox;
  }());

  function xtime(b) { return ((b << 1) ^ ((b & 0x80) ? 0x1b : 0)) & 0xff; }

  function aesExpandKey(key) {
    const nk = key.length / 4;
    const rounds = nk + 6;
    const words = new Uint8Array(16 * (rounds + 1));
    words.set(key, 0);
    let rcon = 1;
    for (let i = nk; i < 4 * (rounds + 1); i++) {
      const at = i * 4;
      let a = words[at - 4];
      let b = words[at - 3];
      let c = words[at - 2];
      let d = words[at - 1];
      if (i % nk === 0) {
        const t = a;
        a = AES_SBOX[b] ^ rcon;
        b = AES_SBOX[c];
        c = AES_SBOX[d];
        d = AES_SBOX[t];
        rcon = xtime(rcon);
      }
      else if (nk > 6 && i % nk === 4) {
        a = AES_SBOX[a]; b = AES_SBOX[b]; c = AES_SBOX[c]; d = AES_SBOX[d];
      }
      words[at] = words[at - nk * 4] ^ a;
      words[at + 1] = words[at - nk * 4 + 1] ^ b;
      words[at + 2] = words[at - nk * 4 + 2] ^ c;
      words[at + 3] = words[at - nk * 4 + 3] ^ d;
    }
    return { schedule: words, rounds: rounds };
  }

  // In place, on a sixteen-byte block.
  function aesEncryptBlock(expanded, block) {
    const s = block;
    const schedule = expanded.schedule;
    const rounds = expanded.rounds;
    for (let i = 0; i < 16; i++) { s[i] ^= schedule[i]; }
    for (let round = 1; round <= rounds; round++) {
      for (let i = 0; i < 16; i++) { s[i] = AES_SBOX[s[i]]; }
      // ShiftRows, on the column-major state OpenPGP and AES both use.
      let t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
      t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
      t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
      if (round !== rounds) {
        for (let c = 0; c < 16; c += 4) {
          const a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
          const all = a0 ^ a1 ^ a2 ^ a3;
          s[c] = a0 ^ all ^ xtime(a0 ^ a1);
          s[c + 1] = a1 ^ all ^ xtime(a1 ^ a2);
          s[c + 2] = a2 ^ all ^ xtime(a2 ^ a3);
          s[c + 3] = a3 ^ all ^ xtime(a3 ^ a0);
        }
      }
      const base = round * 16;
      for (let i = 0; i < 16; i++) { s[i] ^= schedule[base + i]; }
    }
    return s;
  }

  const SYM = { AES128: 7, AES192: 8, AES256: 9 };

  function symKeyLength(id) {
    if (id === SYM.AES128) { return 16; }
    if (id === SYM.AES192) { return 24; }
    if (id === SYM.AES256) { return 32; }
    if (id === 0) { throw new Error('an unencrypted message is not a message this page reads'); }
    const names = { 1: 'IDEA', 2: 'TripleDES', 3: 'CAST5', 4: 'Blowfish', 10: 'Twofish', 11: 'Camellia-128', 12: 'Camellia-192', 13: 'Camellia-256' };
    throw new Error('the cipher is ' + (names[id] || ('algorithm ' + id)) + ', which this page does not have; AES is what it reads');
  }

  function symName(id) {
    if (id === SYM.AES128) { return 'AES-128'; }
    if (id === SYM.AES192) { return 'AES-192'; }
    return 'AES-256';
  }

  // Standard CFB over full blocks, with the tail handled a byte at a time. The
  // secret-key packet and the integrity-protected data packet both use this;
  // the older symmetrically-encrypted data packet adds a resynchronisation,
  // which its own reader does.
  function cfbEncrypt(key, iv, data) {
    const expanded = aesExpandKey(key);
    const feedback = new Uint8Array(iv);
    const out = new Uint8Array(data.length);
    const keystream = new Uint8Array(16);
    for (let at = 0; at < data.length; at += 16) {
      keystream.set(feedback);
      aesEncryptBlock(expanded, keystream);
      const size = Math.min(16, data.length - at);
      for (let i = 0; i < size; i++) { out[at + i] = data[at + i] ^ keystream[i]; }
      feedback.set(out.subarray(at, at + size));
      // A final short block leaves the feedback register unused, which is
      // exactly what a stream cipher mode means by the last block.
    }
    return out;
  }

  function cfbDecrypt(key, iv, data) {
    const expanded = aesExpandKey(key);
    const feedback = new Uint8Array(iv);
    const out = new Uint8Array(data.length);
    const keystream = new Uint8Array(16);
    for (let at = 0; at < data.length; at += 16) {
      keystream.set(feedback);
      aesEncryptBlock(expanded, keystream);
      const size = Math.min(16, data.length - at);
      for (let i = 0; i < size; i++) { out[at + i] = data[at + i] ^ keystream[i]; }
      feedback.set(data.subarray(at, at + size));
    }
    return out;
  }

  // ---- the AES key wrap, through Web Crypto --------------------------------
  //
  // RFC 3394, which RFC 6637 wraps a session key with. Web Crypto has it as
  // "AES-KW" but only between CryptoKeys, so the octets go in and out as a raw
  // HMAC key: the one import that takes an arbitrary length.

  async function aesKeyWrap(kek, data) {
    const wrapping = await subtle.importKey('raw', kek, { name: 'AES-KW' }, false, ['wrapKey']);
    const payload = await subtle.importKey('raw', data, { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']);
    return new Uint8Array(await subtle.wrapKey('raw', payload, wrapping, 'AES-KW'));
  }

  async function aesKeyUnwrap(kek, wrapped) {
    const wrapping = await subtle.importKey('raw', kek, { name: 'AES-KW' }, false, ['unwrapKey']);
    const payload = await subtle.unwrapKey('raw', wrapped, wrapping, 'AES-KW', { name: 'HMAC', hash: 'SHA-256' }, true, ['sign']);
    return new Uint8Array(await subtle.exportKey('raw', payload));
  }

  // ---- string to key (RFC 4880 section 3.7) --------------------------------

  const S2K_SIMPLE = 0;
  const S2K_SALTED = 1;
  const S2K_ITERATED = 3;

  function s2kCount(coded) { return (16 + (coded & 15)) << ((coded >> 4) + 6); }

  function s2kCoded(count) {
    // The smallest coded octet whose count is at least what was asked for.
    for (let coded = 0; coded < 256; coded++) { if (s2kCount(coded) >= count) { return coded; } }
    return 255;
  }

  function readS2k(bytes, at) {
    const type = bytes[at];
    const hash = bytes[at + 1];
    if (type === S2K_SIMPLE) { return { type: type, hash: hash, next: at + 2 }; }
    if (type === S2K_SALTED) { return { type: type, hash: hash, salt: bytes.subarray(at + 2, at + 10), next: at + 10 }; }
    if (type === S2K_ITERATED) { return { type: type, hash: hash, salt: bytes.subarray(at + 2, at + 10), coded: bytes[at + 10], next: at + 11 }; }
    if (type === 4) { throw new Error('an Argon2 string-to-key was met, which belongs to the version 6 format this page does not read'); }
    throw new Error('string-to-key type ' + type + ' is not one this page knows');
  }

  function writeS2k(s2k) {
    if (s2k.type === S2K_ITERATED) { return concat([new Uint8Array([s2k.type, s2k.hash]), s2k.salt, new Uint8Array([s2k.coded])]); }
    if (s2k.type === S2K_SALTED) { return concat([new Uint8Array([s2k.type, s2k.hash]), s2k.salt]); }
    return new Uint8Array([s2k.type, s2k.hash]);
  }

  async function s2kDerive(s2k, passphrase, length) {
    const password = utf8(String(passphrase));
    const salt = s2k.salt || new Uint8Array(0);
    const seed = concat([salt, password]);
    let count = seed.length;
    if (s2k.type === S2K_ITERATED) { count = Math.max(s2kCount(s2k.coded), seed.length); }
    else if (s2k.type === S2K_SIMPLE) { count = password.length; }
    const source = s2k.type === S2K_SIMPLE ? password : seed;
    const out = new Uint8Array(length);
    let written = 0;
    let context = 0;
    while (written < length) {
      const buffer = new Uint8Array(context + count);
      let at = context;
      while (at < buffer.length) {
        const size = Math.min(source.length, buffer.length - at);
        buffer.set(source.subarray(0, size), at);
        at += size;
      }
      const block = await digest(s2k.hash, buffer);
      const size = Math.min(block.length, length - written);
      out.set(block.subarray(0, size), written);
      written += size;
      context++;
    }
    return out;
  }

  // ---- public key algorithms and the two curves ----------------------------

  const ALGO = { RSA: 1, RSA_ENCRYPT: 2, RSA_SIGN: 3, ELGAMAL: 16, DSA: 17, ECDH: 18, ECDSA: 19, EDDSA_LEGACY: 22 };

  // The object identifiers, as their octets appear in a key packet: the curve
  // GnuPG calls ed25519 (1.3.6.1.4.1.11591.15.1) and the one it calls cv25519
  // (1.3.6.1.4.1.3029.1.5.1). A key on any other curve is refused by name.
  const OID_ED25519 = new Uint8Array([0x2b, 0x06, 0x01, 0x04, 0x01, 0xda, 0x47, 0x0f, 0x01]);
  const OID_CV25519 = new Uint8Array([0x2b, 0x06, 0x01, 0x04, 0x01, 0x97, 0x55, 0x01, 0x05, 0x01]);

  const KNOWN_CURVES = [
    { name: 'ed25519', oid: OID_ED25519 },
    { name: 'cv25519', oid: OID_CV25519 }
  ];

  function curveOf(oid) {
    for (const curve of KNOWN_CURVES) { if (equalBytes(curve.oid, oid)) { return curve.name; } }
    const known = { '2a8648ce3d030107': 'NIST P-256', '2b81040022': 'NIST P-384', '2b81040023': 'NIST P-521', '2b2403030208010107': 'Brainpool P-256', '2b240303020801010b': 'Brainpool P-384', '2b240303020801010d': 'Brainpool P-512' };
    const name = known[hex(oid)];
    if (name) { throw new Error('the key is on ' + name + ', and this page has Curve25519 and RSA'); }
    throw new Error('the key is on a curve this page does not know (' + hex(oid) + ')');
  }

  function algorithmName(algorithm, curve, bits) {
    if (algorithm === ALGO.RSA || algorithm === ALGO.RSA_ENCRYPT || algorithm === ALGO.RSA_SIGN) { return 'RSA ' + bits; }
    if (algorithm === ALGO.EDDSA_LEGACY) { return 'Ed25519'; }
    if (algorithm === ALGO.ECDH) { return curve === 'cv25519' ? 'X25519' : ('ECDH ' + curve); }
    if (algorithm === ALGO.ECDSA) { return 'ECDSA ' + curve; }
    if (algorithm === ALGO.DSA) { return 'DSA ' + bits; }
    if (algorithm === ALGO.ELGAMAL) { return 'ElGamal ' + bits; }
    return 'algorithm ' + algorithm;
  }

  // ---- key packets ---------------------------------------------------------

  function parseKeyPacket(body) {
    const version = body[0];
    if (version === 3) { throw new Error('this is a version 3 key, whose fingerprint is not a hash of its material; this page reads version 4 keys'); }
    if (version === 5 || version === 6) { throw new Error('this is a version ' + version + ' key (RFC 9580); this page reads the version 4 keys GnuPG 2.4 makes by default'); }
    if (version !== 4) { throw new Error('key packet version ' + version + ' is not one this page knows'); }
    const key = { version: 4, created: readUint32(body, 1), algorithm: body[5], curve: '', bits: 0, material: {} };
    let at = 6;
    if (key.algorithm === ALGO.RSA || key.algorithm === ALGO.RSA_ENCRYPT || key.algorithm === ALGO.RSA_SIGN) {
      const n = readMpi(body, at);
      const e = readMpi(body, n.next);
      key.material.n = n.bytes;
      key.material.e = e.bytes;
      key.bits = n.bits;
      at = e.next;
    }
    else if (key.algorithm === ALGO.ECDH || key.algorithm === ALGO.ECDSA || key.algorithm === ALGO.EDDSA_LEGACY) {
      const oidLength = body[at++];
      if (!oidLength || oidLength === 0xff) { throw new Error('the key names no curve'); }
      const oid = body.subarray(at, at + oidLength);
      at += oidLength;
      key.curve = curveOf(oid);
      key.oid = oid;
      const point = readMpi(body, at);
      at = point.next;
      // The point is written with a 0x40 prefix and the native 32 octets after
      // it, which is what both Curve25519 algorithms use.
      if (point.bytes.length !== 33 || point.bytes[0] !== 0x40) { throw new Error('the Curve25519 point in the key is not the 0x40-prefixed form'); }
      key.material.point = point.bytes.subarray(1);
      key.bits = 255;
      if (key.algorithm === ALGO.ECDH) {
        const kdfLength = body[at];
        if (kdfLength < 3) { throw new Error('the key\'s agreement parameters are too short'); }
        key.material.kdfHash = body[at + 2];
        key.material.kdfSym = body[at + 3];
        at += 1 + kdfLength;
      }
    }
    else if (key.algorithm === ALGO.DSA) { throw new Error('this is a DSA key, which this page does not have; RSA and Curve25519 are what it reads'); }
    else if (key.algorithm === ALGO.ELGAMAL) { throw new Error('this is an ElGamal key, which this page does not have; RSA and Curve25519 are what it reads'); }
    else if (key.algorithm === 25 || key.algorithm === 27) { throw new Error('this is a version 6 style X25519 or Ed25519 key (algorithm ' + key.algorithm + '); this page reads the version 4 form GnuPG 2.4 writes'); }
    else { throw new Error('public key algorithm ' + key.algorithm + ' is not one this page knows'); }
    key.materialEnd = at;
    key.public = body.subarray(0, at);
    return key;
  }

  // RFC 4880 section 12.2: SHA-1 over 0x99, the two-octet length of the public
  // key material and the material itself. A subkey's is computed the same way
  // over its own packet, which is why this takes the material and not a packet.
  async function fingerprintOf(key) {
    const bytes = await sha1(concat([new Uint8Array([0x99]), uint16(key.public.length), key.public]));
    key.fingerprint = hex(bytes);
    key.keyId = key.fingerprint.slice(-16);
    return key.fingerprint;
  }

  // ---- signature packets ---------------------------------------------------

  const SIGTYPE = {
    BINARY: 0x00, TEXT: 0x01, GENERIC_CERT: 0x10, PERSONA_CERT: 0x11, CASUAL_CERT: 0x12,
    POSITIVE_CERT: 0x13, SUBKEY_BINDING: 0x18, PRIMARY_BINDING: 0x19, DIRECT_KEY: 0x1f,
    KEY_REVOCATION: 0x20, SUBKEY_REVOCATION: 0x28, CERT_REVOCATION: 0x30
  };

  const SUB = {
    CREATED: 2, SIG_EXPIRY: 3, KEY_EXPIRY: 9, PREFERRED_SYM: 11, ISSUER_KEYID: 16,
    PRIMARY_USER_ID: 25, POLICY_URI: 26, KEY_FLAGS: 27, SIGNER_USER_ID: 28,
    REVOCATION_REASON: 29, FEATURES: 30, EMBEDDED_SIGNATURE: 32, ISSUER_FINGERPRINT: 33,
    PREFERRED_HASH: 21, PREFERRED_COMPRESSION: 22, KEY_SERVER_PREFS: 23, PREFERRED_AEAD: 34
  };

  const FLAG = { CERTIFY: 0x01, SIGN: 0x02, ENCRYPT_COMMS: 0x04, ENCRYPT_STORAGE: 0x08, AUTHENTICATE: 0x20 };

  function readSubpackets(bytes) {
    const out = [];
    let at = 0;
    while (at < bytes.length) {
      const first = bytes[at++];
      let length;
      if (first < 192) { length = first; }
      else if (first < 255) { length = ((first - 192) << 8) + bytes[at++] + 192; }
      else { length = readUint32(bytes, at); at += 4; }
      if (!length || at + length > bytes.length) { break; }
      const typeByte = bytes[at];
      out.push({ critical: !!(typeByte & 0x80), type: typeByte & 0x7f, data: bytes.subarray(at + 1, at + length) });
      at += length;
    }
    return out;
  }

  function writeSubpacket(type, data) {
    const body = concat([new Uint8Array([type]), data]);
    return concat([writeLength(body.length), body]);
  }

  function parseSignature(body) {
    const version = body[0];
    if (version === 2 || version === 3) { throw new Error('this is a version ' + version + ' signature, which this page does not read'); }
    if (version === 5 || version === 6) { throw new Error('this is a version ' + version + ' signature (RFC 9580), which this page does not read'); }
    if (version !== 4) { throw new Error('signature packet version ' + version + ' is not one this page knows'); }
    const hashedLength = readUint16(body, 4);
    const signature = {
      version: 4,
      type: body[1],
      publicAlgorithm: body[2],
      hashAlgorithm: body[3],
      trailer: body.subarray(0, 6 + hashedLength),
      hashed: readSubpackets(body.subarray(6, 6 + hashedLength))
    };
    let at = 6 + hashedLength;
    const unhashedLength = readUint16(body, at);
    at += 2;
    signature.unhashed = readSubpackets(body.subarray(at, at + unhashedLength));
    at += unhashedLength;
    signature.left = body.subarray(at, at + 2);
    at += 2;
    signature.mpis = [];
    while (at + 2 <= body.length) {
      const m = readMpi(body, at);
      signature.mpis.push(m.bytes);
      at = m.next;
    }
    signature.body = body;
    return signature;
  }

  function subpacketOf(signature, type, hashedOnly) {
    for (const p of signature.hashed) { if (p.type === type) { return p.data; } }
    if (hashedOnly) { return null; }
    for (const p of signature.unhashed) { if (p.type === type) { return p.data; } }
    return null;
  }

  function signatureCreated(signature) {
    const data = subpacketOf(signature, SUB.CREATED, true);
    return data && data.length >= 4 ? readUint32(data, 0) : 0;
  }

  function signatureIssuer(signature) {
    const fingerprint = subpacketOf(signature, SUB.ISSUER_FINGERPRINT, false);
    if (fingerprint && fingerprint.length === 21) { return hex(fingerprint.subarray(1)).slice(-16); }
    const keyId = subpacketOf(signature, SUB.ISSUER_KEYID, false);
    return keyId && keyId.length === 8 ? hex(keyId) : '';
  }

  function signatureKeyExpiry(signature, created) {
    const data = subpacketOf(signature, SUB.KEY_EXPIRY, true);
    if (!data || data.length < 4) { return 0; }
    const seconds = readUint32(data, 0);
    return seconds ? created + seconds : 0;
  }

  function signatureFlags(signature) {
    const data = subpacketOf(signature, SUB.KEY_FLAGS, true);
    return data && data.length ? data[0] : 0;
  }

  // What is hashed for a signature over data: the data, the signature's own
  // header and subpackets, then the two-octet marker and the length of that
  // header (RFC 4880 section 5.2.4).
  function signatureHashInput(data, trailer) {
    return concat([data, trailer, new Uint8Array([0x04, 0xff]), uint32(trailer.length)]);
  }

  // The prefix a key contributes to a certification or binding signature.
  function keyHashPrefix(key) {
    return concat([new Uint8Array([0x99]), uint16(key.public.length), key.public]);
  }

  function userIdHashPrefix(userId) {
    const bytes = utf8(userId);
    return concat([new Uint8Array([0xb4]), uint32(bytes.length), bytes]);
  }

  // ---- the key material, in and out of Web Crypto --------------------------

  // Whether this browser has Ed25519 and X25519. Chrome 137, Firefox 130,
  // Safari 17 and Node 18 do; an older one is told by name, and RSA still
  // works there. Asked once and remembered, since it costs a key generation.
  let curveProbe = null;
  function curve25519Available() {
    if (!curveProbe) {
      curveProbe = (async function () {
        if (!subtle) { return false; }
        try {
          await subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
          await subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
          return true;
        }
        catch (e) { return false; }
      }());
    }
    return curveProbe;
  }

  // The eighteen fixed octets of a PKCS#8 wrapper around a thirty-two octet
  // Curve25519 private key: SEQUENCE { 0, SEQUENCE { OID }, OCTET STRING {
  // OCTET STRING } }. Only the OID octet differs between the two curves.
  function curvePkcs8(seed, ed) {
    return concat([
      new Uint8Array([0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, ed ? 0x70 : 0x6e, 0x04, 0x22, 0x04, 0x20]),
      seed
    ]);
  }

  function leftPad(bytes, length) {
    if (bytes.length === length) { return bytes; }
    if (bytes.length > length) {
      let start = 0;
      while (start < bytes.length - length && bytes[start] === 0) { start++; }
      if (bytes.length - start !== length) { throw new Error('a number in the key is ' + bytes.length + ' octets where ' + length + ' were expected'); }
      return bytes.subarray(start);
    }
    const out = new Uint8Array(length);
    out.set(bytes, length - bytes.length);
    return out;
  }

  function reversed(bytes) {
    const out = new Uint8Array(bytes.length);
    for (let i = 0; i < bytes.length; i++) { out[i] = bytes[bytes.length - 1 - i]; }
    return out;
  }

  async function importRsaPublic(material, hash) {
    return subtle.importKey('jwk', { kty: 'RSA', n: b64url(material.n), e: b64url(material.e), ext: true },
      { name: 'RSASSA-PKCS1-v1_5', hash: hash }, false, ['verify']);
  }

  async function importRsaPrivate(material, secret, hash) {
    const n = bytesToBig(material.n);
    const e = bytesToBig(material.e);
    const d = bytesToBig(secret.d);
    const p = bytesToBig(secret.p);
    const q = bytesToBig(secret.q);
    const jwk = {
      kty: 'RSA', ext: true,
      n: b64url(bigToBytes(n)), e: b64url(bigToBytes(e)), d: b64url(bigToBytes(d)),
      p: b64url(bigToBytes(p)), q: b64url(bigToBytes(q)),
      dp: b64url(bigToBytes(d % (p - 1n))), dq: b64url(bigToBytes(d % (q - 1n))),
      qi: b64url(bigToBytes(modInverse(q, p)))
    };
    return subtle.importKey('jwk', jwk, { name: 'RSASSA-PKCS1-v1_5', hash: hash }, false, ['sign']);
  }

  // ---- signing and verifying one signature ---------------------------------

  async function verifyWithKey(keyPacket, signature, data) {
    const input = signatureHashInput(data, signature.trailer);
    if (keyPacket.algorithm === ALGO.RSA || keyPacket.algorithm === ALGO.RSA_SIGN) {
      const name = hashName(signature.hashAlgorithm);
      const publicKey = await importRsaPublic(keyPacket.material, name);
      const bytes = leftPad(signature.mpis[0], keyPacket.material.n.length);
      return subtle.verify({ name: 'RSASSA-PKCS1-v1_5' }, publicKey, bytes, input);
    }
    if (keyPacket.algorithm === ALGO.EDDSA_LEGACY) {
      if (!(await curve25519Available())) { throw new Error('this browser has no Ed25519; a signature by a Curve25519 key cannot be checked here'); }
      const publicKey = await subtle.importKey('raw', keyPacket.material.point, { name: 'Ed25519' }, false, ['verify']);
      const bytes = concat([leftPad(signature.mpis[0] || new Uint8Array(0), 32), leftPad(signature.mpis[1] || new Uint8Array(0), 32)]);
      // EdDSA in OpenPGP signs the digest of the data, not the data.
      return subtle.verify({ name: 'Ed25519' }, publicKey, bytes, await digest(signature.hashAlgorithm, input));
    }
    if (keyPacket.algorithm === ALGO.ECDSA) { throw new Error('an ECDSA signature was met, which this page does not check'); }
    if (keyPacket.algorithm === ALGO.DSA) { throw new Error('a DSA signature was met, which this page does not check'); }
    throw new Error('a signature by public key algorithm ' + keyPacket.algorithm + ' was met, which this page does not check');
  }

  // Makes one signature packet. hashedSubpackets and unhashedSubpackets are
  // already-encoded runs of subpackets; data is what the signature covers,
  // before this packet's own trailer is added to it.
  async function makeSignaturePacket(signer, type, hashId, hashedSubpackets, unhashedSubpackets, data) {
    const keyPacket = signer.keyPacket;
    const trailer = concat([
      new Uint8Array([4, type, keyPacket.algorithm, hashId]),
      uint16(hashedSubpackets.length), hashedSubpackets
    ]);
    const input = signatureHashInput(data, trailer);
    const hashBytes = await digest(hashId, input);
    let mpis;
    if (keyPacket.algorithm === ALGO.RSA || keyPacket.algorithm === ALGO.RSA_SIGN) {
      const privateKey = await importRsaPrivate(keyPacket.material, keyPacket.secret, hashName(hashId));
      const bytes = new Uint8Array(await subtle.sign({ name: 'RSASSA-PKCS1-v1_5' }, privateKey, input));
      mpis = writeMpi(bytes);
    }
    else if (keyPacket.algorithm === ALGO.EDDSA_LEGACY) {
      if (!(await curve25519Available())) { throw new Error('this browser has no Ed25519; a Curve25519 key cannot sign here'); }
      const privateKey = await subtle.importKey('pkcs8', curvePkcs8(leftPad(keyPacket.secret.scalar, 32), true), { name: 'Ed25519' }, false, ['sign']);
      const bytes = new Uint8Array(await subtle.sign({ name: 'Ed25519' }, privateKey, hashBytes));
      mpis = concat([writeMpi(bytes.subarray(0, 32)), writeMpi(bytes.subarray(32))]);
    }
    else { throw new Error('this key cannot sign: its algorithm is ' + algorithmName(keyPacket.algorithm, keyPacket.curve, keyPacket.bits)); }
    return writePacket(TAG.SIGNATURE, concat([
      trailer, uint16(unhashedSubpackets.length), unhashedSubpackets,
      hashBytes.subarray(0, 2), mpis
    ]));
  }

  // The subpackets every signature this page makes carries: when it was made,
  // and which key made it, by fingerprint as well as by key id.
  function issuerSubpackets(keyPacket, created) {
    return {
      hashed: concat([
        writeSubpacket(SUB.CREATED, uint32(created)),
        writeSubpacket(SUB.ISSUER_FINGERPRINT, concat([new Uint8Array([4]), fromHex(keyPacket.fingerprint)]))
      ]),
      unhashed: writeSubpacket(SUB.ISSUER_KEYID, fromHex(keyPacket.keyId))
    };
  }

  // ---- secret key material -------------------------------------------------

  function parseSecretTail(body, key) {
    const at = key.materialEnd;
    const usage = body[at];
    const tail = { usage: usage };
    if (usage === 0) {
      tail.plain = body.subarray(at + 1, body.length - 2);
      tail.checksum = body.subarray(body.length - 2);
      return tail;
    }
    if (usage === 253) { throw new Error('the secret key is protected with AEAD (string-to-key usage 253), which belongs to the version 6 format this page does not read'); }
    if (usage !== 254 && usage !== 255) { throw new Error('the secret key uses the obsolete protection of string-to-key usage ' + usage + '; re-export it from GnuPG 2, which writes usage 254'); }
    let cursor = at + 1;
    tail.sym = body[cursor++];
    const s2k = readS2k(body, cursor);
    tail.s2k = s2k;
    cursor = s2k.next;
    tail.iv = body.subarray(cursor, cursor + 16);
    cursor += 16;
    tail.encrypted = body.subarray(cursor);
    return tail;
  }

  function readSecretMpis(key, plain) {
    const secret = {};
    if (key.algorithm === ALGO.RSA || key.algorithm === ALGO.RSA_SIGN || key.algorithm === ALGO.RSA_ENCRYPT) {
      const d = readMpi(plain, 0);
      const p = readMpi(plain, d.next);
      const q = readMpi(plain, p.next);
      const u = readMpi(plain, q.next);
      secret.d = d.bytes; secret.p = p.bytes; secret.q = q.bytes; secret.u = u.bytes;
      secret.end = u.next;
    }
    else {
      const scalar = readMpi(plain, 0);
      secret.scalar = leftPad(scalar.bytes, 32);
      secret.end = scalar.next;
    }
    return secret;
  }

  function writeSecretMpis(key) {
    if (key.algorithm === ALGO.RSA || key.algorithm === ALGO.RSA_SIGN || key.algorithm === ALGO.RSA_ENCRYPT) {
      return concat([writeMpi(key.secret.d), writeMpi(key.secret.p), writeMpi(key.secret.q), writeMpi(key.secret.u)]);
    }
    return writeMpi(key.secret.scalar);
  }

  async function unlockKeyPacket(key, passphrase) {
    const tail = key.tail;
    if (!tail) { throw new Error('this key packet holds no secret material'); }
    let plain;
    if (tail.usage === 0) {
      plain = tail.plain;
      let sum = 0;
      for (let i = 0; i < plain.length; i++) { sum = (sum + plain[i]) & 0xffff; }
      if (readUint16(tail.checksum, 0) !== sum) { throw new Error('the unprotected secret key does not match its checksum'); }
    }
    else {
      if (passphrase === undefined || passphrase === null) { throw new Error('this key needs its passphrase'); }
      const derived = await s2kDerive(tail.s2k, passphrase, symKeyLength(tail.sym));
      const opened = cfbDecrypt(derived, tail.iv, tail.encrypted);
      if (tail.usage === 254) {
        const body = opened.subarray(0, opened.length - 20);
        const given = opened.subarray(opened.length - 20);
        if (!equalBytes(await sha1(body), given)) { throw new Error('wrong passphrase'); }
        plain = body;
      }
      else {
        const body = opened.subarray(0, opened.length - 2);
        let sum = 0;
        for (let i = 0; i < body.length; i++) { sum = (sum + body[i]) & 0xffff; }
        if (readUint16(opened.subarray(opened.length - 2), 0) !== sum) { throw new Error('wrong passphrase'); }
        plain = body;
      }
    }
    key.secret = readSecretMpis(key, plain);
    return key;
  }

  // The secret key packet body as it goes back out: the public material, then
  // the secret numbers under iterated-and-salted S2K and AES-256 in CFB with
  // the SHA-1 checksum of usage 254 - which is what GnuPG 2 writes, so a key
  // this page exports imports there.
  const EXPORT_ITERATIONS = 65011712;

  async function sealSecretKeyPacket(key, passphrase) {
    const material = writeSecretMpis(key);
    if (!passphrase) {
      let sum = 0;
      for (let i = 0; i < material.length; i++) { sum = (sum + material[i]) & 0xffff; }
      return concat([key.public, new Uint8Array([0]), material, uint16(sum)]);
    }
    const s2k = { type: S2K_ITERATED, hash: HASH.SHA256, salt: randomBytes(8), coded: s2kCoded(EXPORT_ITERATIONS) };
    const derived = await s2kDerive(s2k, passphrase, 32);
    const iv = randomBytes(16);
    const plain = concat([material, await sha1(material)]);
    return concat([key.public, new Uint8Array([254, SYM.AES256]), writeS2k(s2k), iv, cfbEncrypt(derived, iv, plain)]);
  }

  // ---- a transferable key --------------------------------------------------

  function addressOfUserId(userId) {
    const m = String(userId).match(/<([^>]*)>/);
    const text = (m ? m[1] : String(userId)).trim().toLowerCase();
    return text.indexOf('@') > 0 ? text : '';
  }

  function nameOfUserId(userId) {
    const m = String(userId).match(/^([^<(]*)/);
    return m ? m[1].trim() : '';
  }

  function decodeUtf8(bytes) { return new TextDecoder('utf-8').decode(bytes); }

  // Four-character groups, as GnuPG and every key page print a fingerprint:
  // a person reading one aloud reads it in fours.
  function fingerprintText(fingerprint) {
    return String(fingerprint || '').toUpperCase().replace(/(.{4})/g, '$1 ').trim();
  }

  // Reads one transferable key - public or secret - and checks every
  // self-signature on it. passphrase, when given, unlocks the secret material
  // at once; without one a secret key is read and stays locked.
  async function readKey(input, passphrase) {
    const bytes = anyToBytes(input);
    const packets = readPackets(bytes);
    if (!packets.length) { throw new Error('there is no packet in this block'); }
    const first = packets[0].tag;
    if (first !== TAG.PUBLIC_KEY && first !== TAG.SECRET_KEY) {
      if (first === TAG.PKESK || first === TAG.SEIPD || first === TAG.SED || first === TAG.SKESK || first === TAG.LITERAL || first === TAG.COMPRESSED) { throw new Error('this is a message, not a key'); }
      if (first === TAG.SIGNATURE) { throw new Error('this is a signature or a revocation certificate, not a key'); }
      throw new Error('a key begins with its primary key packet; this block begins with packet ' + first);
    }

    const key = {
      bytes: bytes, packets: packets, isSecret: first === TAG.SECRET_KEY,
      userIds: [], subkeys: [], revocations: [], directSignatures: [],
      expires: 0, flags: 0, revoked: false, verified: true,
      preferences: { sym: [], hash: [], compression: [], features: 0 }
    };

    const primary = parseKeyPacket(packets[0].body);
    if (key.isSecret) { primary.tail = parseSecretTail(packets[0].body, primary); }
    await fingerprintOf(primary);
    key.primary = primary;
    key.fingerprint = primary.fingerprint;
    key.keyId = primary.keyId;
    key.created = primary.created;
    key.algorithm = algorithmName(primary.algorithm, primary.curve, primary.bits);

    let holder = null;   // the user id or subkey the signatures that follow belong to
    for (let i = 1; i < packets.length; i++) {
      const packet = packets[i];
      if (packet.tag === TAG.USER_ID) {
        holder = { id: decodeUtf8(packet.body), raw: packet.body, signatures: [], revoked: false, primaryUserId: false };
        key.userIds.push(holder);
        continue;
      }
      if (packet.tag === TAG.USER_ATTRIBUTE) {
        // A photo. Kept in the packet list so an export is byte for byte the
        // key that arrived, and otherwise passed over.
        holder = { attribute: true, signatures: [] };
        continue;
      }
      if (packet.tag === TAG.PUBLIC_SUBKEY || packet.tag === TAG.SECRET_SUBKEY) {
        const subkey = parseKeyPacket(packet.body);
        if (packet.tag === TAG.SECRET_SUBKEY) { subkey.tail = parseSecretTail(packet.body, subkey); }
        await fingerprintOf(subkey);
        subkey.signatures = [];
        subkey.revoked = false;
        subkey.flags = 0;
        subkey.expires = 0;
        key.subkeys.push(subkey);
        holder = subkey;
        continue;
      }
      if (packet.tag === TAG.SIGNATURE) {
        const signature = parseSignature(packet.body);
        if (!holder) {
          if (signature.type === SIGTYPE.KEY_REVOCATION) { key.revocations.push(signature); }
          else { key.directSignatures.push(signature); }
          continue;
        }
        holder.signatures.push(signature);
        continue;
      }
      if (packet.tag === TAG.TRUST || packet.tag === TAG.MARKER) { continue; }
      throw new Error('packet ' + packet.tag + ' has no place in a key');
    }

    if (!key.userIds.length) { throw new Error('the key carries no user id'); }
    await judgeKey_(key);
    if (passphrase !== undefined && passphrase !== null) { await unlockKey(key, passphrase); }
    return key;
  }

  // Every self-signature verified, and what it says taken from it: the
  // expiry, the flags, the preferences, the revocations. A signature that does
  // not verify is not believed - it is dropped, and the key is marked as
  // carrying one, which the page shows.
  async function judgeKey_(key) {
    const primary = key.primary;
    const mine = function (signature) { const issuer = signatureIssuer(signature); return !issuer || issuer === primary.keyId; };

    for (const signature of key.revocations) {
      if (!mine(signature)) { continue; }
      if (await checked_(key, function () { return verifyWithKey(primary, signature, keyHashPrefix(primary)); })) { key.revoked = true; }
    }
    for (const signature of key.directSignatures) {
      if (signature.type !== SIGTYPE.DIRECT_KEY || !mine(signature)) { continue; }
      if (!(await checked_(key, function () { return verifyWithKey(primary, signature, keyHashPrefix(primary)); }))) { continue; }
      const expires = signatureKeyExpiry(signature, primary.created);
      if (expires) { key.expires = expires; }
    }

    let best = null;
    for (const userId of key.userIds) {
      for (const signature of userId.signatures) {
        if (!mine(signature)) { continue; }
        const isCertification = signature.type >= SIGTYPE.GENERIC_CERT && signature.type <= SIGTYPE.POSITIVE_CERT;
        const isRevocation = signature.type === SIGTYPE.CERT_REVOCATION;
        if (!isCertification && !isRevocation) { continue; }
        const data = concat([keyHashPrefix(primary), userIdHashPrefix(userId.id)]);
        if (!(await checked_(key, function () { return verifyWithKey(primary, signature, data); }))) { continue; }
        if (isRevocation) { userId.revoked = true; continue; }
        const created = signatureCreated(signature);
        if (!userId.signature || created >= userId.created) {
          userId.signature = signature;
          userId.created = created;
          userId.primaryUserId = !!subpacketOf(signature, SUB.PRIMARY_USER_ID, true);
        }
      }
      if (!userId.signature) { continue; }
      if (!best || (userId.primaryUserId && !best.primaryUserId) || (userId.primaryUserId === best.primaryUserId && userId.created > best.created)) { best = userId; }
    }
    if (!best) { throw new Error('no user id on this key carries a self-signature this page could check'); }

    key.userId = best.id;
    key.name = nameOfUserId(best.id);
    key.address = addressOfUserId(best.id);
    key.addresses = key.userIds.filter(function (u) { return u.id && !u.revoked && addressOfUserId(u.id); }).map(function (u) { return addressOfUserId(u.id); });
    key.flags = signatureFlags(best.signature) || (FLAG.CERTIFY | FLAG.SIGN);
    if (!key.expires) { key.expires = signatureKeyExpiry(best.signature, primary.created); }
    const sym = subpacketOf(best.signature, SUB.PREFERRED_SYM, true);
    const hashes = subpacketOf(best.signature, SUB.PREFERRED_HASH, true);
    const compression = subpacketOf(best.signature, SUB.PREFERRED_COMPRESSION, true);
    const features = subpacketOf(best.signature, SUB.FEATURES, true);
    key.preferences.sym = sym ? Array.from(sym) : [];
    key.preferences.hash = hashes ? Array.from(hashes) : [];
    key.preferences.compression = compression ? Array.from(compression) : [];
    key.preferences.features = features && features.length ? features[0] : 0;

    for (const subkey of key.subkeys) {
      for (const signature of subkey.signatures) {
        if (!mine(signature)) { continue; }
        if (signature.type !== SIGTYPE.SUBKEY_BINDING && signature.type !== SIGTYPE.SUBKEY_REVOCATION) { continue; }
        const data = concat([keyHashPrefix(primary), keyHashPrefix(subkey)]);
        if (!(await checked_(key, function () { return verifyWithKey(primary, signature, data); }))) { continue; }
        if (signature.type === SIGTYPE.SUBKEY_REVOCATION) { subkey.revoked = true; continue; }
        const created = signatureCreated(signature);
        if (!subkey.signature || created >= signatureCreated(subkey.signature)) {
          subkey.signature = signature;
          let flags = signatureFlags(signature);
          // A subkey that may SIGN has to bind the primary back (RFC 4880
          // section 5.2.3.26, the embedded primary-key binding): without it,
          // anyone could append their own subkey to somebody's genuine key -
          // the primary fingerprint the reader checks unchanged - and sign as
          // them. No back signature, or one that does not verify, and the
          // subkey keeps every other flag but may not sign.
          if ((flags & FLAG.SIGN) && !(await crossCertified_(key, primary, subkey, signature))) { flags &= ~FLAG.SIGN; }
          subkey.flags = flags;
          subkey.expires = signatureKeyExpiry(signature, subkey.created);
        }
      }
    }
  }

  // Runs one verification and answers whether it said yes. A self-signature
  // that does not verify, and one that cannot be checked here - an algorithm
  // this page does not have, a browser without Ed25519 - both leave the key
  // marked as not fully verified, which a signature badge made with it says.
  async function checked_(key, run) {
    try {
      const ok = await run();
      if (!ok) { key.verified = false; }
      return ok;
    }
    catch (e) { key.verified = false; return false; }
  }

  // The embedded primary-key binding a signing subkey's binding signature must
  // carry: a PRIMARY_BINDING signature made by the SUBKEY over the primary and
  // the subkey, in the binding's hashed or unhashed subpackets. True only when
  // it is there and verifies.
  async function crossCertified_(key, primary, subkey, binding) {
    const embedded = subpacketOf(binding, SUB.EMBEDDED_SIGNATURE, false);
    if (!embedded || !embedded.length) { return false; }
    let back;
    try { back = parseSignature(embedded); } catch (e) { return false; }
    if (back.type !== SIGTYPE.PRIMARY_BINDING) { return false; }
    const data = concat([keyHashPrefix(primary), keyHashPrefix(subkey)]);
    return checked_(key, function () { return verifyWithKey(subkey, back, data); });
  }

  function keyExpired(key, at) {
    const when = at === undefined ? Math.floor(Date.now() / 1000) : at;
    return !!(key.expires && key.expires < when);
  }

  // Whether this key may be used at all: not revoked, not expired.
  function keyUsable(key) { return !key.revoked && !keyExpired(key); }

  function keyProblem(key) {
    if (key.revoked) { return 'the key is revoked'; }
    if (keyExpired(key)) { return 'the key expired'; }
    return '';
  }

  // The packet that signs: the primary when it may, else a subkey that may.
  function signingKeyPacket(key) {
    if ((key.flags & FLAG.SIGN) && (key.primary.algorithm === ALGO.RSA || key.primary.algorithm === ALGO.RSA_SIGN || key.primary.algorithm === ALGO.EDDSA_LEGACY)) { return key.primary; }
    let found = null;
    for (const subkey of key.subkeys) {
      if (subkey.revoked || !(subkey.flags & FLAG.SIGN) || (subkey.expires && subkey.expires < Math.floor(Date.now() / 1000))) { continue; }
      if (!found || subkey.created > found.created) { found = subkey; }
    }
    return found;
  }

  // The packet that encrypts: the newest living subkey that may, else the
  // primary when it may - an RSA key with no subkey, which is what a key made
  // by an old client looks like.
  function encryptionKeyPacket(key) {
    let found = null;
    for (const subkey of key.subkeys) {
      if (subkey.revoked || !(subkey.flags & (FLAG.ENCRYPT_COMMS | FLAG.ENCRYPT_STORAGE))) { continue; }
      if (subkey.expires && subkey.expires < Math.floor(Date.now() / 1000)) { continue; }
      if (subkey.algorithm !== ALGO.RSA && subkey.algorithm !== ALGO.RSA_ENCRYPT && subkey.algorithm !== ALGO.ECDH) { continue; }
      if (!found || subkey.created > found.created) { found = subkey; }
    }
    if (found) { return found; }
    if ((key.flags & (FLAG.ENCRYPT_COMMS | FLAG.ENCRYPT_STORAGE)) && (key.primary.algorithm === ALGO.RSA || key.primary.algorithm === ALGO.RSA_ENCRYPT)) { return key.primary; }
    return null;
  }

  // Unlocks every secret packet the key holds. A key whose primary opens and
  // whose subkey does not is a key half in hand, so both are tried and the
  // first refusal is the answer.
  async function unlockKey(key, passphrase) {
    if (!key.isSecret) { throw new Error('this is a public key; there is nothing to unlock'); }
    await unlockKeyPacket(key.primary, passphrase);
    for (const subkey of key.subkeys) { if (subkey.tail) { await unlockKeyPacket(subkey, passphrase); } }
    key.unlocked = true;
    return key;
  }

  // ---- writing a key out ---------------------------------------------------

  // The public half of whatever was read: a secret key packet becomes the
  // public key packet its first octets already are, and everything else - the
  // user ids, the certifications, the bindings - goes out byte for byte, since
  // those are the bytes the signatures were made over.
  function publicPacketsOf(key) {
    const out = [];
    let subkeyIndex = 0;
    for (const packet of key.packets) {
      if (packet.tag === TAG.SECRET_KEY) { out.push(writePacket(TAG.PUBLIC_KEY, key.primary.public)); continue; }
      if (packet.tag === TAG.SECRET_SUBKEY) { out.push(writePacket(TAG.PUBLIC_SUBKEY, key.subkeys[subkeyIndex++].public)); continue; }
      if (packet.tag === TAG.PUBLIC_SUBKEY) { subkeyIndex++; }
      if (packet.tag === TAG.TRUST) { continue; }
      out.push(writePacket(packet.tag, packet.body));
    }
    return concat(out);
  }

  function exportPublic(key) { return armor('PUBLIC KEY BLOCK', publicPacketsOf(key), { Comment: key.fingerprint.toUpperCase() }); }
  function publicBytes(key) { return publicPacketsOf(key); }

  // The secret key, sealed again under the passphrase given. Every packet that
  // is not a key packet goes out as it came in, so the user ids and the
  // signatures are byte for byte what was signed.
  async function exportSecret(key, passphrase) {
    if (!key.isSecret) { throw new Error('this is a public key; there is no secret key to export'); }
    if (!key.unlocked) { throw new Error('the key is locked; unlock it before exporting it'); }
    const out = [];
    let subkeyIndex = 0;
    for (const packet of key.packets) {
      if (packet.tag === TAG.SECRET_KEY) { out.push(writePacket(TAG.SECRET_KEY, await sealSecretKeyPacket(key.primary, passphrase))); continue; }
      if (packet.tag === TAG.SECRET_SUBKEY) { out.push(writePacket(TAG.SECRET_SUBKEY, await sealSecretKeyPacket(key.subkeys[subkeyIndex++], passphrase))); continue; }
      if (packet.tag === TAG.PUBLIC_SUBKEY) { subkeyIndex++; }
      if (packet.tag === TAG.TRUST) { continue; }
      out.push(writePacket(packet.tag, packet.body));
    }
    return armor('PRIVATE KEY BLOCK', concat(out), { Comment: key.fingerprint.toUpperCase() });
  }

  // Several transferable keys in one block: what "gpg --armor --export" writes
  // when it is given more than one name, and what a keyring file is. The
  // packets are split at each primary key packet and each group is read as a
  // key of its own. A group is rebuilt with new packet headers, which changes
  // nothing that matters - a fingerprint is a hash of a packet's BODY, and a
  // signature covers bodies too - and a secret key's passphrase is offered only
  // to a group that begins with a secret key packet, so a file holding one of
  // each opens with one call.
  async function readKeys(input, passphrase) {
    const packets = readPackets(anyToBytes(input));
    const groups = [];
    for (const packet of packets) {
      if (packet.tag === TAG.PUBLIC_KEY || packet.tag === TAG.SECRET_KEY) { groups.push([]); }
      if (!groups.length) { continue; }
      groups[groups.length - 1].push(packet);
    }
    if (!groups.length) { throw new Error('there is no key in this block'); }
    const out = [];
    for (const group of groups) {
      const bytes = concat(group.map(function (p) { return writePacket(p.tag, p.body); }));
      out.push(await readKey(bytes, group[0].tag === TAG.SECRET_KEY ? passphrase : undefined));
    }
    return out;
  }

  // ---- making a key --------------------------------------------------------

  // The preferences a key this page makes announces: AES-256 first, SHA-512
  // first, ZLIB offered and the modification detection code required.
  const PREFERRED_SYM = new Uint8Array([SYM.AES256, SYM.AES192, SYM.AES128]);
  const PREFERRED_HASH = new Uint8Array([HASH.SHA512, HASH.SHA384, HASH.SHA256]);
  const PREFERRED_COMPRESSION = new Uint8Array([2, 1, 0]);

  async function newKeyPacket_(created, algorithm, publicFields, secret) {
    const body = concat([new Uint8Array([4]), uint32(created), new Uint8Array([algorithm]), publicFields]);
    const key = parseKeyPacket(body);
    key.secret = secret;
    await fingerprintOf(key);
    return key;
  }

  async function newEd25519_(created) {
    const pair = await subtle.generateKey({ name: 'Ed25519' }, true, ['sign', 'verify']);
    const point = new Uint8Array(await subtle.exportKey('raw', pair.publicKey));
    const pkcs8 = new Uint8Array(await subtle.exportKey('pkcs8', pair.privateKey));
    const seed = new Uint8Array(pkcs8.subarray(pkcs8.length - 32));
    const fields = concat([new Uint8Array([OID_ED25519.length]), OID_ED25519, writeMpi(concat([new Uint8Array([0x40]), point]))]);
    return newKeyPacket_(created, ALGO.EDDSA_LEGACY, fields, { scalar: seed });
  }

  async function newX25519_(created) {
    const pair = await subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
    const point = new Uint8Array(await subtle.exportKey('raw', pair.publicKey));
    const pkcs8 = new Uint8Array(await subtle.exportKey('pkcs8', pair.privateKey));
    const scalar = new Uint8Array(pkcs8.subarray(pkcs8.length - 32));
    // Clamped here so that what is stored is what X25519 would have used
    // anyway, and is byte for byte what GnuPG stores for the same key.
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
    const fields = concat([
      new Uint8Array([OID_CV25519.length]), OID_CV25519,
      writeMpi(concat([new Uint8Array([0x40]), point])),
      // The agreement parameters GnuPG writes for this curve: three octets,
      // the reserved 1, SHA-256 as the KDF hash and AES-128 as the wrap.
      new Uint8Array([3, 1, HASH.SHA256, SYM.AES128])
    ]);
    // The scalar goes into the packet big-endian, which is the one place
    // OpenPGP differs from X25519's own little-endian convention.
    return newKeyPacket_(created, ALGO.ECDH, fields, { scalar: reversed(scalar) });
  }

  async function newRsa_(created, bits, algorithm) {
    const pair = await subtle.generateKey({ name: 'RSASSA-PKCS1-v1_5', modulusLength: bits, publicExponent: new Uint8Array([1, 0, 1]), hash: 'SHA-256' }, true, ['sign', 'verify']);
    const jwk = await subtle.exportKey('jwk', pair.privateKey);
    let p = bytesToBig(b64urlDecode(jwk.p));
    let q = bytesToBig(b64urlDecode(jwk.q));
    if (p > q) { const swap = p; p = q; q = swap; }
    const fields = concat([writeMpi(b64urlDecode(jwk.n)), writeMpi(b64urlDecode(jwk.e))]);
    return newKeyPacket_(created, algorithm, fields, {
      d: b64urlDecode(jwk.d), p: bigToBytes(p), q: bigToBytes(q), u: bigToBytes(modInverse(p, q))
    });
  }

  // spec: { type: 'curve25519' | 'rsa', bits, name, email, expiresIn (seconds
  // from now; 0 never), passphrase }. Answers the key, unlocked, with its
  // armoured public and secret forms and a revocation certificate made at the
  // same moment - which is the only time it can be made without the passphrase
  // being typed again, and the one thing a person needs if the key is lost.
  async function generateKey(spec) {
    const type = (spec.type || 'curve25519').toLowerCase();
    const created = Math.floor(Date.now() / 1000);
    const userId = (spec.name ? spec.name + ' ' : '') + '<' + String(spec.email || '').trim() + '>';
    if (!addressOfUserId(userId)) { throw new Error('a key needs an e-mail address'); }
    if (/[<>]/.test(String(spec.name || ''))) { throw new Error('the name may not hold angle brackets'); }

    let primary;
    let subkey;
    if (type === 'rsa') {
      const bits = Number(spec.bits) || 3072;
      if (bits !== 2048 && bits !== 3072 && bits !== 4096) { throw new Error('an RSA key here is 2048, 3072 or 4096 bits'); }
      primary = await newRsa_(created, bits, ALGO.RSA);
      subkey = await newRsa_(created, bits, ALGO.RSA);
    }
    else if (type === 'curve25519') {
      if (!(await curve25519Available())) { throw new Error('this browser has no Ed25519 or X25519; choose RSA, or use a browser that has them (Chrome 137, Firefox 130, Safari 17 or later)'); }
      primary = await newEd25519_(created);
      subkey = await newX25519_(created);
    }
    else { throw new Error('a key here is curve25519 or rsa'); }

    const expiresIn = Math.max(0, Math.floor(Number(spec.expiresIn) || 0));
    const signer = { keyPacket: primary };
    const issuer = issuerSubpackets(primary, created);

    const certHashed = concat([
      issuer.hashed,
      expiresIn ? writeSubpacket(SUB.KEY_EXPIRY, uint32(expiresIn)) : new Uint8Array(0),
      writeSubpacket(SUB.KEY_FLAGS, new Uint8Array([FLAG.CERTIFY | FLAG.SIGN])),
      writeSubpacket(SUB.PREFERRED_SYM, PREFERRED_SYM),
      writeSubpacket(SUB.PREFERRED_HASH, PREFERRED_HASH),
      writeSubpacket(SUB.PREFERRED_COMPRESSION, PREFERRED_COMPRESSION),
      writeSubpacket(SUB.FEATURES, new Uint8Array([0x01])),
      writeSubpacket(SUB.KEY_SERVER_PREFS, new Uint8Array([0x80])),
      writeSubpacket(SUB.PRIMARY_USER_ID, new Uint8Array([1]))
    ]);
    const userIdBytes = utf8(userId);
    const certification = await makeSignaturePacket(signer, SIGTYPE.POSITIVE_CERT, HASH.SHA256, certHashed, issuer.unhashed,
      concat([keyHashPrefix(primary), userIdHashPrefix(userId)]));

    const bindHashed = concat([
      issuer.hashed,
      expiresIn ? writeSubpacket(SUB.KEY_EXPIRY, uint32(expiresIn)) : new Uint8Array(0),
      writeSubpacket(SUB.KEY_FLAGS, new Uint8Array([FLAG.ENCRYPT_COMMS | FLAG.ENCRYPT_STORAGE]))
    ]);
    const binding = await makeSignaturePacket(signer, SIGTYPE.SUBKEY_BINDING, HASH.SHA256, bindHashed, issuer.unhashed,
      concat([keyHashPrefix(primary), keyHashPrefix(subkey)]));

    const secretBytes = concat([
      writePacket(TAG.SECRET_KEY, await sealSecretKeyPacket(primary, spec.passphrase)),
      writePacket(TAG.USER_ID, userIdBytes),
      certification,
      writePacket(TAG.SECRET_SUBKEY, await sealSecretKeyPacket(subkey, spec.passphrase)),
      binding
    ]);
    const key = await readKey(secretBytes, spec.passphrase);
    const revocation = await revocationCertificate(key, 0, 'Made with the key, in case it is ever lost.');
    return { key: key, publicArmored: exportPublic(key), secretArmored: await exportSecret(key, spec.passphrase), revocation: revocation, fingerprint: key.fingerprint };
  }

  // ---- revocation ----------------------------------------------------------
  //
  // A revocation is a signature by the key over itself saying it is finished.
  // It is made here, armoured as a public key block - which is what GnuPG
  // writes and what GnuPG imports - and it may be made now and kept for later,
  // which is the only way to revoke a key whose passphrase has been forgotten.

  const REVOCATION_REASONS = { 0: 'no reason given', 1: 'the key was superseded', 2: 'the key was compromised', 3: 'the key is no longer used' };

  async function revocationCertificate(key, reasonCode, reasonText) {
    if (!key.isSecret || !key.unlocked) { throw new Error('a revocation is made by the key itself; unlock it first'); }
    const created = Math.floor(Date.now() / 1000);
    const issuer = issuerSubpackets(key.primary, created);
    const code = Number(reasonCode) || 0;
    const hashed = concat([
      issuer.hashed,
      writeSubpacket(SUB.REVOCATION_REASON, concat([new Uint8Array([code]), utf8(String(reasonText || ''))]))
    ]);
    const packet = await makeSignaturePacket({ keyPacket: key.primary }, SIGTYPE.KEY_REVOCATION, HASH.SHA256, hashed, issuer.unhashed, keyHashPrefix(key.primary));
    return armor('PUBLIC KEY BLOCK', packet, { Comment: 'This is a revocation certificate' });
  }

  // Reads a revocation certificate against a key: it has to be a key
  // revocation made by that key and it has to verify. Answers a key with the
  // revocation in its packets, so that exporting it carries the revocation to
  // everyone who fetches it.
  async function applyRevocation(key, certificate) {
    const packets = readPackets(anyToBytes(certificate));
    const found = packets.filter(function (p) { return p.tag === TAG.SIGNATURE; });
    if (!found.length) { throw new Error('this is not a revocation certificate: it holds no signature'); }
    let applied = 0;
    for (const packet of found) {
      const signature = parseSignature(packet.body);
      if (signature.type !== SIGTYPE.KEY_REVOCATION) { continue; }
      const issuer = signatureIssuer(signature);
      if (issuer && issuer !== key.primary.keyId) { throw new Error('this revocation was made by key ' + issuer.toUpperCase() + ', not by this one'); }
      if (!(await verifyWithKey(key.primary, signature, keyHashPrefix(key.primary)))) { throw new Error('this revocation is not signed by the key it revokes'); }
      key.packets.splice(1, 0, packet);
      key.revocations.push(signature);
      key.revoked = true;
      applied++;
    }
    if (!applied) { throw new Error('this certificate revokes no key'); }
    return key;
  }

  function revocationReasonOf(key) {
    for (const signature of key.revocations) {
      const data = subpacketOf(signature, SUB.REVOCATION_REASON, true);
      if (!data || !data.length) { continue; }
      const text = data.length > 1 ? decodeUtf8(data.subarray(1)) : '';
      return { code: data[0], reason: REVOCATION_REASONS[data[0]] || 'reason ' + data[0], text: text };
    }
    return key.revoked ? { code: 0, reason: REVOCATION_REASONS[0], text: '' } : null;
  }

  // ---- session keys: the public-key encrypted session key packet -----------

  function sessionChecksum(key) {
    let sum = 0;
    for (let i = 0; i < key.length; i++) { sum = (sum + key[i]) & 0xffff; }
    return sum;
  }

  function pkcs5Pad(bytes) {
    const pad = 8 - (bytes.length % 8);
    const out = new Uint8Array(bytes.length + pad);
    out.set(bytes, 0);
    out.fill(pad, bytes.length);
    return out;
  }

  function pkcs5Unpad(bytes) {
    const pad = bytes[bytes.length - 1];
    if (!pad || pad > 8 || pad > bytes.length) { throw new Error('the wrapped session key is not padded as RFC 6637 says'); }
    for (let i = bytes.length - pad; i < bytes.length; i++) { if (bytes[i] !== pad) { throw new Error('the wrapped session key is not padded as RFC 6637 says'); } }
    return bytes.subarray(0, bytes.length - pad);
  }

  // RFC 6637 section 8: the key encryption key, from the agreed point, the
  // parameters written into the recipient's key and that key's fingerprint.
  async function ecdhKek_(shared, keyPacket) {
    const param = concat([
      new Uint8Array([keyPacket.oid.length]), keyPacket.oid,
      new Uint8Array([ALGO.ECDH, 0x03, 0x01, keyPacket.material.kdfHash, keyPacket.material.kdfSym]),
      utf8('Anonymous Sender    '),
      fromHex(keyPacket.fingerprint)
    ]);
    const bytes = await digest(keyPacket.material.kdfHash, concat([new Uint8Array([0, 0, 0, 1]), shared, param]));
    return bytes.subarray(0, symKeyLength(keyPacket.material.kdfSym));
  }

  async function writePkesk(keyPacket, symAlgorithm, sessionKey) {
    const material = concat([new Uint8Array([symAlgorithm]), sessionKey, uint16(sessionChecksum(sessionKey))]);
    let payload;
    if (keyPacket.algorithm === ALGO.RSA || keyPacket.algorithm === ALGO.RSA_ENCRYPT) {
      const n = bytesToBig(keyPacket.material.n);
      const e = bytesToBig(keyPacket.material.e);
      const size = keyPacket.material.n.length;
      if (material.length + 11 > size) { throw new Error('the recipient\'s RSA key is too small for a session key'); }
      const padding = new Uint8Array(size - material.length - 3);
      cryptoApi.getRandomValues(padding);
      for (let i = 0; i < padding.length; i++) { while (padding[i] === 0) { padding[i] = randomBytes(1)[0]; } }
      const block = concat([new Uint8Array([0x00, 0x02]), padding, new Uint8Array([0x00]), material]);
      payload = writeMpi(bigToBytes(modPow(bytesToBig(block), e, n), size));
    }
    else if (keyPacket.algorithm === ALGO.ECDH) {
      if (keyPacket.curve !== 'cv25519') { throw new Error('the recipient\'s agreement key is on ' + keyPacket.curve + ', and this page has Curve25519'); }
      if (!(await curve25519Available())) { throw new Error('this browser has no X25519; a Curve25519 recipient cannot be encrypted to here'); }
      const ephemeral = await subtle.generateKey({ name: 'X25519' }, true, ['deriveBits']);
      const point = new Uint8Array(await subtle.exportKey('raw', ephemeral.publicKey));
      const recipient = await subtle.importKey('raw', keyPacket.material.point, { name: 'X25519' }, false, []);
      const shared = new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: recipient }, ephemeral.privateKey, 256));
      const kek = await ecdhKek_(shared, keyPacket);
      const wrapped = await aesKeyWrap(kek, pkcs5Pad(material));
      payload = concat([writeMpi(concat([new Uint8Array([0x40]), point])), new Uint8Array([wrapped.length]), wrapped]);
    }
    else { throw new Error('the recipient\'s key cannot receive a message: it is ' + algorithmName(keyPacket.algorithm, keyPacket.curve, keyPacket.bits)); }
    return writePacket(TAG.PKESK, concat([new Uint8Array([3]), fromHex(keyPacket.keyId), new Uint8Array([keyPacket.algorithm]), payload]));
  }

  function readPkeskHeader(body) {
    if (body[0] !== 3) { throw new Error('session key packet version ' + body[0] + ' is not one this page reads'); }
    return { keyId: hex(body.subarray(1, 9)), algorithm: body[9], at: 10 };
  }

  async function readPkesk(body, keyPacket) {
    const head = readPkeskHeader(body);
    let material;
    if (head.algorithm === ALGO.RSA || head.algorithm === ALGO.RSA_ENCRYPT) {
      const c = readMpi(body, head.at);
      const size = keyPacket.material.n.length;
      const n = bytesToBig(keyPacket.material.n);
      const d = bytesToBig(keyPacket.secret.d);
      const p = bytesToBig(keyPacket.secret.p);
      const q = bytesToBig(keyPacket.secret.q);
      const u = bytesToBig(keyPacket.secret.u);
      const value = bytesToBig(c.bytes) % n;
      // The Chinese remainder theorem with OpenPGP's u, which is the inverse
      // of p modulo q - the other way round from PKCS#1's coefficient.
      const m1 = modPow(value, d % (p - 1n), p);
      const m2 = modPow(value, d % (q - 1n), q);
      let h = (u * (((m2 - m1) % q) + q)) % q;
      const plain = bigToBytes(m1 + p * h, size);
      if (plain[0] !== 0x00 || plain[1] !== 0x02) { throw new Error('this message was not encrypted to this key'); }
      let at = 2;
      while (at < plain.length && plain[at] !== 0x00) { at++; }
      if (at >= plain.length || at < 10) { throw new Error('this message was not encrypted to this key'); }
      material = plain.subarray(at + 1);
    }
    else if (head.algorithm === ALGO.ECDH) {
      if (!(await curve25519Available())) { throw new Error('this browser has no X25519; a message to a Curve25519 key cannot be opened here'); }
      const point = readMpi(body, head.at);
      if (point.bytes.length !== 33 || point.bytes[0] !== 0x40) { throw new Error('the sender\'s ephemeral point is not the 0x40-prefixed Curve25519 form'); }
      const wrappedLength = body[point.next];
      const wrapped = body.subarray(point.next + 1, point.next + 1 + wrappedLength);
      const privateKey = await subtle.importKey('pkcs8', curvePkcs8(reversed(leftPad(keyPacket.secret.scalar, 32)), false), { name: 'X25519' }, false, ['deriveBits']);
      const sender = await subtle.importKey('raw', point.bytes.subarray(1), { name: 'X25519' }, false, []);
      const shared = new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: sender }, privateKey, 256));
      const kek = await ecdhKek_(shared, keyPacket);
      material = pkcs5Unpad(await aesKeyUnwrap(kek, wrapped));
    }
    else { throw new Error('the message was encrypted with public key algorithm ' + head.algorithm + ', which this page does not open'); }

    const symAlgorithm = material[0];
    const length = symKeyLength(symAlgorithm);
    const sessionKey = material.subarray(1, 1 + length);
    if (sessionChecksum(sessionKey) !== readUint16(material, 1 + length)) { throw new Error('the session key does not match its checksum'); }
    return { symAlgorithm: symAlgorithm, sessionKey: sessionKey };
  }

  // ---- the symmetric-key encrypted session key packet ----------------------
  //
  // Written and read so that row 152's password-protected message reuses it:
  // a passphrase, not a key, opens the message.

  async function writeSkesk(passphrase, symAlgorithm, sessionKey) {
    const s2k = { type: S2K_ITERATED, hash: HASH.SHA256, salt: randomBytes(8), coded: s2kCoded(EXPORT_ITERATIONS) };
    const derived = await s2kDerive(s2k, passphrase, symKeyLength(symAlgorithm));
    const wrapped = cfbEncrypt(derived, new Uint8Array(16), concat([new Uint8Array([symAlgorithm]), sessionKey]));
    return writePacket(TAG.SKESK, concat([new Uint8Array([4, symAlgorithm]), writeS2k(s2k), wrapped]));
  }

  async function readSkesk(body, passphrase) {
    if (body[0] !== 4) { throw new Error('symmetric session key packet version ' + body[0] + ' is not one this page reads'); }
    const symAlgorithm = body[1];
    const s2k = readS2k(body, 2);
    const derived = await s2kDerive(s2k, passphrase, symKeyLength(symAlgorithm));
    if (s2k.next >= body.length) { return { symAlgorithm: symAlgorithm, sessionKey: derived }; }
    const opened = cfbDecrypt(derived, new Uint8Array(16), body.subarray(s2k.next));
    const inner = opened[0];
    // A wrong passphrase decrypts to noise, and the first octet of noise is
    // an algorithm number only one time in eighty-five: the rest of the time
    // it is a number this page has no cipher for, and the right thing to say
    // about that is not "the cipher is IDEA".
    if (inner !== SYM.AES128 && inner !== SYM.AES192 && inner !== SYM.AES256) { throw new Error('wrong passphrase'); }
    const length = symKeyLength(inner);
    if (opened.length !== 1 + length) { throw new Error('wrong passphrase'); }
    return { symAlgorithm: inner, sessionKey: opened.subarray(1) };
  }

  // ---- the encrypted data packets ------------------------------------------

  // The prefix RFC 4880 section 5.7 puts in front of the plaintext: a block of
  // random octets with its last two repeated, which is what a reader checks
  // before it trusts a session key at all.
  async function sealSeipd(symAlgorithm, sessionKey, inner) {
    if (sessionKey.length !== symKeyLength(symAlgorithm)) { throw new Error('the session key is the wrong length for ' + symName(symAlgorithm)); }
    const prefix = new Uint8Array(18);
    prefix.set(randomBytes(16), 0);
    prefix[16] = prefix[14];
    prefix[17] = prefix[15];
    const plain = concat([prefix, inner, new Uint8Array([0xd3, 0x14])]);
    const full = concat([plain, await sha1(plain)]);
    return writePacket(TAG.SEIPD, concat([new Uint8Array([1]), cfbEncrypt(sessionKey, new Uint8Array(16), full)]));
  }

  async function openSeipd(body, symAlgorithm, sessionKey) {
    if (body[0] !== 1) { throw new Error('the encrypted packet is version ' + body[0] + '; this page reads version 1, which is what AES-CFB messages are'); }
    if (sessionKey.length !== symKeyLength(symAlgorithm)) { throw new Error('the session key is the wrong length for ' + symName(symAlgorithm)); }
    const plain = cfbDecrypt(sessionKey, new Uint8Array(16), body.subarray(1));
    if (plain.length < 18 + 22) { throw new Error('the encrypted packet is too short to hold a message'); }
    if (plain[16] !== plain[14] || plain[17] !== plain[15]) { throw new Error('the message did not open: the session key is not this message\'s'); }
    const marker = plain.subarray(plain.length - 22, plain.length - 20);
    if (marker[0] !== 0xd3 || marker[1] !== 0x14) { throw new Error('the message carries no modification detection code, and this page will not read one without'); }
    const given = plain.subarray(plain.length - 20);
    const computed = await sha1(plain.subarray(0, plain.length - 20));
    if (!equalBytes(given, computed)) { throw new Error('the message was altered after it was encrypted: its modification detection code does not match'); }
    return plain.subarray(18, plain.length - 22);
  }

  // The older packet, read only: the same CFB with the one resynchronisation
  // that only this packet does, and no integrity check at all - which is why
  // nothing writes one any more, and why what comes out of one is reported as
  // unprotected rather than quietly shown as though it had been checked.
  function openSed(body, symAlgorithm, sessionKey) {
    if (sessionKey.length !== symKeyLength(symAlgorithm)) { throw new Error('the session key is the wrong length for ' + symName(symAlgorithm)); }
    if (body.length < 18) { throw new Error('the encrypted packet is too short to hold a message'); }
    const head = cfbDecrypt(sessionKey, new Uint8Array(16), body.subarray(0, 18));
    if (head[16] !== head[14] || head[17] !== head[15]) { throw new Error('the message did not open: the session key is not this message\'s'); }
    return cfbDecrypt(sessionKey, body.subarray(2, 18), body.subarray(18));
  }

  // ---- literal data, compressed data, one-pass signatures ------------------

  function writeLiteral(bytes, filename, when) {
    const name = utf8(String(filename || ''));
    if (name.length > 255) { throw new Error('the file name in a literal packet is at most 255 octets'); }
    return writePacket(TAG.LITERAL, concat([
      new Uint8Array([0x62]), new Uint8Array([name.length]), name, uint32(Math.floor((when || Date.now()) / 1000)), bytes
    ]));
  }

  function readLiteral(body) {
    const format = String.fromCharCode(body[0]);
    const nameLength = body[1];
    const name = decodeUtf8(body.subarray(2, 2 + nameLength));
    const when = readUint32(body, 2 + nameLength);
    return { format: format, filename: name, when: when, bytes: body.subarray(2 + nameLength + 4) };
  }

  // The most a compressed packet is inflated to in the reader's tab: 32 MB,
  // the size of message the server itself reads whole (MaxMessageParseBytes
  // in RestApiServer.cpp). A few kilobytes of deflate can stand for
  // gigabytes, and up to four layers are unwrapped, each held to this; a
  // message past it is refused in so many words rather than inflated until
  // the tab runs out of memory.
  const INFLATED_MOST = 32 * 1024 * 1024;

  // How much of the compressed input goes to the decompressor at a time.
  // Chromium's DecompressionStream inflates the whole of a chunk it is given
  // before the page reads any of the output - backpressure or not - so the
  // ceiling above holds the tab only if the input arrives in pieces: deflate
  // expands at most about 1,032 times, so 16 KB is some 16.5 MB of output at
  // most, and the next piece goes in only once that output has been read and
  // counted.
  const INFLATE_SLICE = 16 * 1024;

  async function inflate_(bytes, raw) {
    if (typeof DecompressionStream === 'undefined') { throw new Error('this browser cannot decompress; the message is compressed'); }
    const stream = new DecompressionStream(raw ? 'deflate-raw' : 'deflate');
    const writer = stream.writable.getWriter();
    const reader = stream.readable.getReader();
    const chunks = [];
    let total = 0;
    let tooLarge = false;
    const reading = (async function () {
      for (;;) {
        const step = await reader.read();
        if (step.done) { return; }
        total += step.value.length;
        if (total > INFLATED_MOST) {
          tooLarge = true;
          try { await reader.cancel(); } catch (e) { /* the stream is dropped either way */ }
          return;
        }
        chunks.push(step.value);
      }
    })();
    // Handled here and awaited below: a decompression error rejects it
    // while a write is still being waited on.
    reading.catch(function () {});
    try {
      for (let at = 0; at < bytes.length && !tooLarge; at += INFLATE_SLICE) {
        await writer.write(bytes.subarray(at, Math.min(bytes.length, at + INFLATE_SLICE)));
      }
      if (!tooLarge) { await writer.close(); }
    } catch (e) {
      // A cancelled read or a decompression error errors the writing side
      // too; the reading side says which.
    }
    await reading;
    if (tooLarge) { throw new Error('the message is larger than 32 MB once decompressed, more than this page opens; open it in a mail program'); }
    return concat(chunks);
  }

  async function readCompressed(body) {
    const algorithm = body[0];
    if (algorithm === 0) { return body.subarray(1); }
    if (algorithm === 1) { return inflate_(body.subarray(1), true); }
    if (algorithm === 2) { return inflate_(body.subarray(1), false); }
    if (algorithm === 3) { throw new Error('the message is compressed with BZip2, which this page does not have; ask the sender\'s client for ZLIB or none'); }
    throw new Error('compression algorithm ' + algorithm + ' is not one this page knows');
  }

  function writeOnePass(keyPacket, sigType, hashId) {
    return writePacket(TAG.ONE_PASS, concat([
      new Uint8Array([3, sigType, hashId, keyPacket.algorithm]), fromHex(keyPacket.keyId), new Uint8Array([1])
    ]));
  }

  // ---- signing and verifying a message -------------------------------------

  // Every line ending CRLF, which is what a signature over text covers and
  // what a MIME part crossing the internet has to be anyway.
  function canonical(bytes) {
    const out = [];
    for (let i = 0; i < bytes.length; i++) {
      if (bytes[i] === 13) {
        out.push(13, 10);
        if (bytes[i + 1] === 10) { i++; }
        continue;
      }
      if (bytes[i] === 10) { out.push(13, 10); continue; }
      out.push(bytes[i]);
    }
    return new Uint8Array(out);
  }

  // The packet a key id names. A subkey counts only once its binding
  // signature verified - an unbound packet in a key blob is not part of the
  // key - and, when the packet is wanted for checking a signature, only when
  // its binding lets it sign; the primary may always be asked.
  function findKeyPacket(keys, keyId, forSigning) {
    for (const key of keys || []) {
      if (!key || !key.primary) { continue; }
      if (key.primary.keyId === keyId) { return { key: key, packet: key.primary }; }
      for (const subkey of key.subkeys) {
        if (subkey.keyId !== keyId) { continue; }
        if (!subkey.signature || subkey.revoked) { continue; }
        if (forSigning && !(subkey.flags & FLAG.SIGN)) { continue; }
        return { key: key, packet: subkey };
      }
    }
    return null;
  }

  function allKeyPackets(keys) {
    const out = [];
    for (const key of keys || []) {
      if (!key || !key.primary) { continue; }
      out.push({ key: key, packet: key.primary });
      for (const subkey of key.subkeys) { out.push({ key: key, packet: subkey }); }
    }
    return out;
  }

  // A detached signature over data, armoured: what PGP/MIME's
  // application/pgp-signature part holds.
  async function signDetached(key, data, hashId) {
    if (!key.unlocked) { throw new Error('the signing key is locked'); }
    const problem = keyProblem(key);
    if (problem) { throw new Error(problem + ': it cannot sign'); }
    const keyPacket = signingKeyPacket(key);
    if (!keyPacket) { throw new Error('this key has nothing that may sign'); }
    if (!keyPacket.secret) { throw new Error('the signing part of this key is locked'); }
    const created = Math.floor(Date.now() / 1000);
    const issuer = issuerSubpackets(keyPacket, created);
    const hashed = concat([issuer.hashed, writeSubpacket(SUB.SIGNER_USER_ID, utf8(key.userId || ''))]);
    const packet = await makeSignaturePacket({ keyPacket: keyPacket }, SIGTYPE.BINARY, hashId || HASH.SHA256, hashed, issuer.unhashed, canonical(data));
    return { packet: packet, armored: armor('SIGNATURE', packet), hash: hashId || HASH.SHA256 };
  }

  // One signature packet, against the data and the keys in hand. Never throws
  // for a signature that simply does not verify: valid is false and error says
  // what happened, which is what a badge in the reader needs.
  async function verifyOneSignature(signature, data, keys) {
    const issuer = signatureIssuer(signature);
    // valid is the CRYPTOGRAPHY and nothing else: whether these bytes were
    // signed by that key. problem is everything else a reader has to be told
    // about a signature that does verify - a key revoked since, a key that had
    // already expired when it was used - because calling a good signature
    // invalid would be a lie about the arithmetic, and saying nothing would be
    // worse.
    const out = {
      valid: false, error: '', problem: '', keyId: issuer, fingerprint: '', key: null,
      created: signatureCreated(signature), signerUserId: '', hash: signature.hashAlgorithm, type: signature.type
    };
    const signerUserId = subpacketOf(signature, SUB.SIGNER_USER_ID, true);
    if (signerUserId) { out.signerUserId = decodeUtf8(signerUserId); }
    const found = issuer ? findKeyPacket(keys, issuer, true) : null;
    if (!found) {
      out.error = issuer ? ('no key here is ' + issuer.toUpperCase()) : 'the signature does not say which key made it';
      return out;
    }
    out.key = found.key;
    out.fingerprint = found.packet.fingerprint;
    try {
      const covered = signature.type === SIGTYPE.TEXT ? canonical(data) : data;
      out.valid = await verifyWithKey(found.packet, signature, covered);
      if (!out.valid) { out.error = 'the signature does not match the message'; }
      else if (found.key.revoked) { out.problem = 'the signing key has been revoked since'; }
      else if (keyExpired(found.key, out.created)) { out.problem = 'the signing key had already expired when this was signed'; }
      else if (found.key.verified === false) { out.problem = 'not every self-signature on the signing key could be checked here'; }
    }
    catch (e) { out.error = e && e.message ? e.message : String(e); }
    return out;
  }

  // A detached signature against the data it should cover. RFC 3156 says the
  // part crossing the internet is CRLF, and every client writes it that way -
  // but a message that reached this server over a path that rewrote its line
  // endings would then fail a signature that is perfectly good, so the bytes
  // as they are are tried first and the canonical form after them.
  async function verifyDetached(signatureInput, data, keys) {
    const packets = readPackets(anyToBytes(signatureInput));
    const signatures = packets.filter(function (p) { return p.tag === TAG.SIGNATURE; });
    if (!signatures.length) { return { valid: false, error: 'the signature part holds no signature' }; }
    const forms = [data];
    const canonicalForm = canonical(data);
    if (!equalBytes(canonicalForm, data)) { forms.push(canonicalForm); }
    let last = null;
    for (const packet of signatures) {
      for (const form of forms) {
        const result = await verifyOneSignature(parseSignature(packet.body), form, keys);
        if (result.valid) { return result; }
        last = last || result;
      }
    }
    return last;
  }

  // ---- encrypting and decrypting a message ---------------------------------

  // The strongest cipher every recipient said they take, AES-256 when they all
  // did or said nothing. AES-128 is the one every implementation must have, so
  // it is the floor rather than a refusal.
  function chooseCipher(keys) {
    const wanted = [SYM.AES256, SYM.AES192, SYM.AES128];
    for (const candidate of wanted) {
      let all = true;
      for (const key of keys) {
        const list = key.preferences && key.preferences.sym.length ? key.preferences.sym : [SYM.AES128];
        if (list.indexOf(candidate) < 0) { all = false; break; }
      }
      if (all) { return candidate; }
    }
    return SYM.AES128;
  }

  // spec: { data, filename, recipients: [key], signer: key|null, passwords: [text] }
  async function encryptBytes(spec) {
    const recipients = spec.recipients || [];
    const passwords = spec.passwords || [];
    if (!recipients.length && !passwords.length) { throw new Error('a message has to be encrypted to somebody'); }
    for (const key of recipients) {
      const problem = keyProblem(key);
      if (problem) { throw new Error(problem + ': ' + (key.address || key.fingerprint) + ' cannot be written to'); }
      if (!encryptionKeyPacket(key)) { throw new Error('the key of ' + (key.address || key.fingerprint) + ' has nothing that may encrypt'); }
    }
    const symAlgorithm = recipients.length ? chooseCipher(recipients) : SYM.AES256;
    const sessionKey = randomBytes(symKeyLength(symAlgorithm));

    let inner = writeLiteral(spec.data, spec.filename || '', Date.now());
    if (spec.signer) {
      const key = spec.signer;
      if (!key.unlocked) { throw new Error('the signing key is locked'); }
      const problem = keyProblem(key);
      if (problem) { throw new Error(problem + ': it cannot sign'); }
      const keyPacket = signingKeyPacket(key);
      if (!keyPacket || !keyPacket.secret) { throw new Error('this key has nothing that may sign'); }
      const created = Math.floor(Date.now() / 1000);
      const issuer = issuerSubpackets(keyPacket, created);
      const hashed = concat([issuer.hashed, writeSubpacket(SUB.SIGNER_USER_ID, utf8(key.userId || ''))]);
      const signature = await makeSignaturePacket({ keyPacket: keyPacket }, SIGTYPE.BINARY, HASH.SHA256, hashed, issuer.unhashed, spec.data);
      inner = concat([writeOnePass(keyPacket, SIGTYPE.BINARY, HASH.SHA256), inner, signature]);
    }

    const heads = [];
    for (const key of recipients) { heads.push(await writePkesk(encryptionKeyPacket(key), symAlgorithm, sessionKey)); }
    for (const password of passwords) { heads.push(await writeSkesk(password, symAlgorithm, sessionKey)); }
    return concat([concat(heads), await sealSeipd(symAlgorithm, sessionKey, inner)]);
  }

  // Opens a message with whatever is in hand: the account's unlocked keys, or
  // a password. Answers the literal data, what it was encrypted to, and the
  // signature inside it when there was one.
  async function decryptBytes(input, keys, passwords) {
    const bytes = anyToBytes(input);
    let packets = readPackets(bytes);
    const out = { data: null, signed: null, keyId: '', symAlgorithm: 0, integrityProtected: true, filename: '', encrypted: true };

    const sessions = packets.filter(function (p) { return p.tag === TAG.PKESK || p.tag === TAG.SKESK; });
    const bodyPacket = packets.filter(function (p) { return p.tag === TAG.SEIPD || p.tag === TAG.SED; })[0];
    if (!bodyPacket) {
      if (!sessions.length) {
        // Not an encrypted message at all: a signed one, or a literal.
        out.encrypted = false;
        return readOpenedPackets_(packets, keys, out);
      }
      throw new Error('the message says it is encrypted and carries nothing encrypted');
    }

    let opened = null;
    let openedByPassword = false;
    let lastError = '';
    for (const packet of sessions) {
      if (packet.tag === TAG.PKESK) {
        const head = readPkeskHeader(packet.body);
        const candidates = head.keyId === '0000000000000000' ? allKeyPackets(keys) : (findKeyPacket(keys, head.keyId) ? [findKeyPacket(keys, head.keyId)] : []);
        for (const candidate of candidates) {
          if (!candidate.packet.secret) { lastError = lastError || 'the key this message was encrypted to is locked'; continue; }
          try { opened = await readPkesk(packet.body, candidate.packet); out.keyId = candidate.packet.keyId; break; }
          catch (e) { lastError = e && e.message ? e.message : String(e); }
        }
      }
      else {
        for (const password of passwords || []) {
          try { opened = await readSkesk(packet.body, password); openedByPassword = true; break; }
          catch (e) { lastError = e && e.message ? e.message : String(e); }
        }
      }
      if (opened) { break; }
    }
    if (!opened) {
      const ids = sessions.filter(function (p) { return p.tag === TAG.PKESK; }).map(function (p) { return readPkeskHeader(p.body).keyId.toUpperCase(); });
      throw new Error(lastError || (ids.length ? ('this message is encrypted to ' + ids.join(', ') + ', and no key here is one of them') : 'no password opened this message'));
    }
    out.symAlgorithm = opened.symAlgorithm;
    try {
      let plain;
      if (bodyPacket.tag === TAG.SEIPD) { plain = await openSeipd(bodyPacket.body, opened.symAlgorithm, opened.sessionKey); }
      else { out.integrityProtected = false; plain = openSed(bodyPacket.body, opened.symAlgorithm, opened.sessionKey); }
      return await readOpenedPackets_(readPackets(plain), keys, out);
    }
    catch (e) {
      // A session key a password gave is not proof that the password was right.
      // A wrong one decrypts a version 4 SKESK's session key to noise, and one
      // time in 256 the noise's first octet is the algorithm number whose key
      // length fits, so readSkesk hands back a key; and an SKESK that carries no
      // session key of its own - gpg -c's - hands back whatever the password
      // derives, with nothing to check it against. Either way it is the body
      // that refuses the key. The error is marked so that a caller who opened
      // with a password alone (openProtected) can say which it was.
      if (openedByPassword && e && typeof e === 'object') { e.sessionKeyFromPassword = true; }
      throw e;
    }
  }

  // What is inside, once the encryption is off: a literal, perhaps compressed,
  // perhaps with a one-pass signature around it.
  async function readOpenedPackets_(packets, keys, out) {
    let list = packets;
    for (let depth = 0; depth < 4; depth++) {
      const compressed = list.filter(function (p) { return p.tag === TAG.COMPRESSED; })[0];
      if (!compressed) { break; }
      list = readPackets(await readCompressed(compressed.body));
      out.compressed = true;
    }
    const literal = list.filter(function (p) { return p.tag === TAG.LITERAL; })[0];
    if (!literal) { throw new Error('there is no message inside: no literal data packet'); }
    const read = readLiteral(literal.body);
    out.data = read.bytes;
    out.filename = read.filename;
    const signature = list.filter(function (p) { return p.tag === TAG.SIGNATURE; })[0];
    if (signature) {
      const parsed = parseSignature(signature.body);
      // A text-mode signature covers the canonical form of the literal data.
      out.signed = await verifyOneSignature(parsed, parsed.type === SIGTYPE.TEXT ? canonical(read.bytes) : read.bytes, keys);
    }
    return out;
  }

  // ---- the cleartext signature framework (read only) -----------------------

  function readCleartext(text) {
    const source = String(text).replace(/\r\n/g, '\n');
    const begin = source.indexOf('-----BEGIN PGP SIGNED MESSAGE-----');
    if (begin < 0) { return null; }
    const afterHeader = source.indexOf('\n\n', begin);
    if (afterHeader < 0) { return null; }
    const sigAt = source.indexOf('-----BEGIN PGP SIGNATURE-----', afterHeader);
    if (sigAt < 0) { return null; }
    const body = source.slice(afterHeader + 2, sigAt);
    const lines = body.split('\n');
    if (lines.length && lines[lines.length - 1] === '') { lines.pop(); }
    const cleaned = lines.map(function (line) {
      const undashed = line.indexOf('- ') === 0 ? line.slice(2) : line;
      return undashed.replace(/[ \t]+$/, '');
    });
    const end = source.indexOf('-----END PGP SIGNATURE-----', sigAt);
    return {
      text: cleaned.join('\n'),
      signed: utf8(cleaned.join('\r\n')),
      signature: source.slice(sigAt, end < 0 ? source.length : end + '-----END PGP SIGNATURE-----'.length)
    };
  }

  // ---- Autocrypt (Level 1) -------------------------------------------------
  //
  // One header on an outgoing message carries the sender's public key, so that
  // a correspondent's client learns it without anybody being asked to fetch
  // anything; the same header read on an incoming message is how this page
  // learns theirs. The key in the header is the minimal one the specification
  // asks for - the primary key, one user id with its self-signature, and the
  // living subkeys with their bindings - and not the whole key with every
  // third-party certification that has ever accreted on it.

  function minimalPublicBytes(key) {
    const out = [writePacket(TAG.PUBLIC_KEY, key.primary.public)];
    let best = null;
    for (const userId of key.userIds) { if (userId.id === key.userId) { best = userId; break; } }
    best = best || key.userIds[0];
    out.push(writePacket(TAG.USER_ID, best.raw || utf8(best.id)));
    if (best.signature) { out.push(writePacket(TAG.SIGNATURE, best.signature.body)); }
    for (const subkey of key.subkeys) {
      if (subkey.revoked || !subkey.signature) { continue; }
      out.push(writePacket(TAG.PUBLIC_SUBKEY, subkey.public));
      out.push(writePacket(TAG.SIGNATURE, subkey.signature.body));
    }
    return concat(out);
  }

  // The header value, folded: a base64 run of several kilobytes on one line
  // would be past the 998 octets RFC 5322 allows, and every Autocrypt
  // implementation unfolds and strips whitespace out of keydata before
  // reading it.
  function autocryptHeader(address, key, preferEncrypt) {
    const data = b64encode(minimalPublicBytes(key));
    let value = 'addr=' + String(address).toLowerCase();
    if (preferEncrypt) { value += '; prefer-encrypt=mutual'; }
    value += '; keydata=';
    let out = value;
    for (let at = 0; at < data.length; at += 72) { out += '\r\n ' + data.slice(at, at + 72); }
    return out;
  }

  // Answers null for a header this page must ignore, which Autocrypt Level 1
  // spells out: no addr, no keydata, or an attribute this version does not
  // know whose name does not begin with an underscore.
  function parseAutocrypt(value) {
    if (!value) { return null; }
    const text = String(value).replace(/\r?\n[ \t]+/g, ' ');
    const attributes = {};
    let unknown = false;
    text.split(';').forEach(function (piece) {
      const eq = piece.indexOf('=');
      if (eq < 0) { if (piece.trim()) { unknown = true; } return; }
      const name = piece.slice(0, eq).trim().toLowerCase();
      const data = piece.slice(eq + 1).trim();
      if (name === 'addr' || name === 'prefer-encrypt' || name === 'keydata') { attributes[name] = data; return; }
      if (name[0] !== '_') { unknown = true; }
    });
    if (unknown || !attributes.addr || !attributes.keydata) { return null; }
    let bytes;
    try { bytes = b64decode(attributes.keydata.replace(/\s+/g, '')); }
    catch (e) { return null; }
    if (!bytes.length) { return null; }
    return {
      address: attributes.addr.toLowerCase(),
      preferEncrypt: (attributes['prefer-encrypt'] || '').toLowerCase() === 'mutual',
      keydata: bytes
    };
  }

  function autocryptOf(entity) {
    const found = entity.headers.filter(function (h) { return h.name.toLowerCase() === 'autocrypt'; });
    // More than one, and the specification says to ignore them all.
    if (found.length !== 1) { return null; }
    return parseAutocrypt(found[0].value);
  }

  // ---- PGP/MIME (RFC 3156) -------------------------------------------------

  const PGP_MIME_SIGNED = 'multipart/signed';
  const PGP_MIME_ENCRYPTED = 'multipart/encrypted';

  function protocolOf(entity) { return String(entity.contentType.params.protocol || '').toLowerCase(); }

  function boundaryText() { return '=_pgp_' + hex(randomBytes(12)); }

  // Whether this message has anything for this module to do, which is what the
  // page asks before it fetches the module at all.
  function looksLikeOpenPgp(headerText, bodyText) {
    const type = String(headerText || '').match(/^content-type:\s*([^\r\n]*(?:\r?\n[ \t][^\r\n]*)*)/im);
    const value = type ? type[1].replace(/\r?\n[ \t]+/g, ' ').toLowerCase() : '';
    if (value.indexOf('application/pgp-encrypted') >= 0 || value.indexOf('application/pgp-signature') >= 0 || value.indexOf('application/pgp-keys') >= 0) { return true; }
    if (/^autocrypt:/im.test(String(headerText || ''))) { return true; }
    return /-----BEGIN PGP (MESSAGE|SIGNED MESSAGE|PUBLIC KEY BLOCK)-----/.test(String(bodyText || ''));
  }

  // The whole reader's side: a message in, and what the page shows out. keys
  // are every key in hand - the account's own, unlocked, for the decryption,
  // and the correspondents' for the signature.
  async function readMessage(input, keys, passwords) {
    const m = mime();
    let bytes = input instanceof Uint8Array ? input : utf8(String(input));
    let entity = m.parseMime(bytes);
    const out = { layers: [], encrypted: null, signed: null, content: null, entity: entity, autocrypt: autocryptOf(entity), inline: false, integrityProtected: true };

    for (let depth = 0; depth < 4; depth++) {
      const type = entity.contentType.type;
      if (type === PGP_MIME_ENCRYPTED && protocolOf(entity) === 'application/pgp-encrypted' && entity.parts && entity.parts.length >= 2) {
        out.layers.push('encrypted');
        let opened;
        try { opened = await decryptBytes(m.decodeBody(entity.parts[1]), keys, passwords); }
        catch (e) { out.encrypted = { ok: false, keyId: '', error: e && e.message ? e.message : String(e) }; break; }
        out.encrypted = { ok: true, keyId: opened.keyId, error: '' };
        out.integrityProtected = opened.integrityProtected;
        if (opened.signed) { out.signed = opened.signed; out.layers.push('signed'); }
        bytes = opened.data;
        entity = m.parseMime(bytes);
        continue;
      }
      if (type === PGP_MIME_SIGNED && protocolOf(entity) === 'application/pgp-signature' && entity.parts && entity.parts.length >= 2) {
        out.layers.push('signed');
        const covered = bytes.subarray(entity.parts[0].start, entity.parts[0].end);
        try { out.signed = await verifyDetached(m.decodeBody(entity.parts[1]), covered, keys); }
        catch (e) { out.signed = { valid: false, error: e && e.message ? e.message : String(e) }; }
        entity = entity.parts[0];
        continue;
      }
      break;
    }

    out.entity = entity;
    if (out.encrypted && !out.encrypted.ok) { return out; }
    out.content = m.extractContent(entity);

    // Inline PGP, read only: a message written by a client that never adopted
    // PGP/MIME, or one written by a person pasting a block into the body.
    if (!out.encrypted && out.content && out.content.text) {
      const inline = await readInline_(out.content.text, keys, passwords);
      if (inline) {
        out.inline = true;
        if (inline.encrypted) { out.encrypted = inline.encrypted; out.layers.push('encrypted'); }
        if (inline.signed) { out.signed = inline.signed; out.layers.push('signed'); }
        if (inline.text !== undefined) { out.content.text = inline.text; out.content.html = ''; }
      }
    }
    return out;
  }

  async function readInline_(text, keys, passwords) {
    const cleartext = readCleartext(text);
    if (cleartext) {
      let signed;
      try { signed = await verifyDetached(cleartext.signature, cleartext.signed, keys); }
      catch (e) { signed = { valid: false, error: e && e.message ? e.message : String(e) }; }
      return { signed: signed, text: cleartext.text };
    }
    const begin = text.indexOf('-----BEGIN PGP MESSAGE-----');
    if (begin < 0) { return null; }
    const end = text.indexOf('-----END PGP MESSAGE-----', begin);
    if (end < 0) { return null; }
    const block = text.slice(begin, end + '-----END PGP MESSAGE-----'.length);
    try {
      const opened = await decryptBytes(block, keys, passwords);
      const inner = new TextDecoder('utf-8').decode(opened.data);
      return { encrypted: { ok: true, keyId: opened.keyId, error: '' }, signed: opened.signed, text: inner };
    }
    catch (e) {
      return { encrypted: { ok: false, keyId: '', error: e && e.message ? e.message : String(e) } };
    }
  }

  // The writer's side. spec: { headers, text, html, attachments, sign (an
  // unlocked key or null), encrypt (an array of keys or null) }. Answers the
  // whole message as text, exactly as the S/MIME module does, so the page
  // hands the server the same shape whichever format it used.
  async function composeMessage(spec) {
    const m = mime();
    let entity = m.buildMime(spec);
    if (spec.encrypt && spec.encrypt.length) {
      const message = await encryptBytes({ data: canonical(entity), filename: '', recipients: spec.encrypt, signer: spec.sign || null, passwords: spec.passwords || [] });
      const b = boundaryText();
      entity = utf8('Content-Type: multipart/encrypted; protocol="application/pgp-encrypted"; boundary="' + b + '"\r\n\r\n'
        + 'This is an OpenPGP/MIME encrypted message (RFC 3156).\r\n\r\n'
        + '--' + b + '\r\nContent-Type: application/pgp-encrypted\r\nContent-Description: PGP/MIME version identification\r\n\r\nVersion: 1\r\n\r\n'
        + '--' + b + '\r\nContent-Type: application/octet-stream; name="encrypted.asc"\r\nContent-Description: OpenPGP encrypted message\r\nContent-Disposition: inline; filename="encrypted.asc"\r\n\r\n'
        + armor('MESSAGE', message)
        + '--' + b + '--\r\n');
    }
    else if (spec.sign) {
      const part = canonical(entity);
      const signature = await signDetached(spec.sign, part, HASH.SHA256);
      const b = boundaryText();
      entity = concat([
        utf8('Content-Type: multipart/signed; micalg=pgp-sha256; protocol="application/pgp-signature"; boundary="' + b + '"\r\n\r\n'
          + 'This is an OpenPGP/MIME signed message (RFC 3156).\r\n\r\n--' + b + '\r\n'),
        part,
        utf8('\r\n--' + b + '\r\nContent-Type: application/pgp-signature; name="signature.asc"\r\nContent-Description: OpenPGP digital signature\r\nContent-Disposition: attachment; filename="signature.asc"\r\n\r\n'
          + signature.armored
          + '--' + b + '--\r\n')
      ]);
    }
    return m.messageHeaders(spec.headers || {}) + binaryString(entity);
  }

  // ---- a message to a password (Roadmap2 row 152) --------------------------
  //
  // For the correspondent who has no key: the text and the attachments are
  // built into one MIME message - headers, then the entity the S/MIME module
  // builds, exactly as a message this page sends - and that message is the
  // literal packet of ONE OpenPGP message encrypted to a passphrase (a
  // symmetric-key encrypted session key and an integrity-protected body:
  // RFC 4880 sections 5.3 and 5.13, which GnuPG's "gpg -c" writes and reads).
  // The armour is what the server files and what the reading page fetches;
  // the password never leaves the browser it was typed in.

  // spec: { headers, text, html, attachments, password }. Answers the armour.
  async function protectedMessage(spec) {
    const password = String(spec && spec.password || '');
    if (!password) { throw new Error('a password is needed'); }
    const m = mime();
    const entity = m.buildMime({ text: spec.text || '', html: spec.html || '', attachments: spec.attachments || [] });
    const message = concat([utf8(m.messageHeaders(spec.headers || {})), canonical(entity)]);
    const sealed = await encryptBytes({ data: message, filename: 'message.eml', recipients: [], passwords: [password] });
    return armor('MESSAGE', sealed);
  }

  // The armour and the password in; the message's headers that matter and
  // its content - text, html and attachments as bytes - out. Whatever went
  // wrong is thrown, and the page that shows a reader the result says "that
  // password is not right" for all of it: a wrong password and a damaged
  // message are not the reader's to tell apart.
  async function openProtected(input, password) {
    let opened;
    try { opened = await decryptBytes(input, [], [String(password || '')]); }
    catch (e) {
      // No key is offered, so a session key can only have come from the
      // password; if the message then did not open with it, the password was
      // wrong - or the message is damaged, which the reader is not the one to
      // tell apart either. Without this, a wrong password answered "the
      // session key is not this message's" one time in 256 (decryptBytes).
      if (e && e.sessionKeyFromPassword) { throw new Error('wrong passphrase'); }
      throw e;
    }
    // decryptBytes reads a bare literal - no encryption at all - and the old
    // unprotected data packet, and says so in two flags a reader could
    // ignore. A message that was never encrypted, or was encrypted without
    // integrity protection, is not one this page shows as opened with a
    // password: it is refused in the one sentence the page says for a
    // wrong one, because the reader is not the one to tell them apart.
    if (!opened.encrypted || !opened.integrityProtected) { throw new Error('wrong passphrase'); }
    const m = mime();
    const entity = m.parseMime(opened.data);
    const content = m.extractContent(entity);
    return {
      subject: m.decodeHeaderValue(entity.header('Subject') || ''),
      from: m.decodeHeaderValue(entity.header('From') || ''),
      to: m.decodeHeaderValue(entity.header('To') || ''),
      date: entity.header('Date') || '',
      content: content,
      integrityProtected: opened.integrityProtected
    };
  }

  // ---- BLAKE2b (RFC 7693) and Argon2 (RFC 9106) -----------------------------
  //
  // A zero-access mailbox (docs/ZeroAccess.md) seals its key under a
  // passphrase derived from the account password with Argon2id, and the Web
  // Crypto API has no Argon2 - PBKDF2 and HKDF are all it offers - so it is
  // here: BLAKE2b, which Argon2 is built on, and the three Argon2 variants,
  // in some three hundred lines on Uint32Array pairs, because BigInt is
  // fifty times too slow for a function whose whole point is to be slow in
  // memory rather than in arithmetic. Every 64-bit word is two 32-bit halves,
  // low first, as the RFC's little-endian layout has them anyway.
  //
  // Proved against the RFCs' own vectors in build/check-portal-openpgp.js:
  // BLAKE2b-512 of "abc" from RFC 7693 appendix A, and the Argon2d, Argon2i
  // and Argon2id vectors of RFC 9106 sections 5.1 to 5.3, secret and
  // associated data included, so that the indexing of all three variants is
  // held to the RFC and not to this file agreeing with itself.
  //
  // NOT constant time, and said so: it runs in the person's own browser on
  // the person's own password, where an attacker who could measure it is
  // already running code in that browser.

  const BLAKE2B_IV = new Uint32Array([
    0xf3bcc908, 0x6a09e667, 0x84caa73b, 0xbb67ae85, 0xfe94f82b, 0x3c6ef372, 0x5f1d36f1, 0xa54ff53a,
    0xade682d1, 0x510e527f, 0x2b3e6c1f, 0x9b05688c, 0xfb41bd6b, 0x1f83d9ab, 0x137e2179, 0x5be0cd19
  ]);
  const BLAKE2B_SIGMA = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3]
  ];

  // The BLAKE2b compression function F over the working vector v (32 words:
  // 16 pairs), the message m (32 words) and the counter, as the RFC lays it
  // out. h, v and m are Uint32Arrays of pairs; the mixing function G is
  // written out in place, with the 64-bit adds and rotations on the halves.
  function blake2bCompress(h, m, v, t0, t1, last) {
    for (let i = 0; i < 16; i++) { v[i] = h[i]; v[i + 16] = BLAKE2B_IV[i]; }
    v[24] ^= t0; v[25] ^= t1;
    if (last) { v[28] = ~v[28]; v[29] = ~v[29]; }
    for (let r = 0; r < 12; r++) {
      const s = BLAKE2B_SIGMA[r];
      blake2bG(v, m, 0, 8, 16, 24, s[0], s[1]);
      blake2bG(v, m, 2, 10, 18, 26, s[2], s[3]);
      blake2bG(v, m, 4, 12, 20, 28, s[4], s[5]);
      blake2bG(v, m, 6, 14, 22, 30, s[6], s[7]);
      blake2bG(v, m, 0, 10, 20, 30, s[8], s[9]);
      blake2bG(v, m, 2, 12, 22, 24, s[10], s[11]);
      blake2bG(v, m, 4, 14, 16, 26, s[12], s[13]);
      blake2bG(v, m, 6, 8, 18, 28, s[14], s[15]);
    }
    for (let i = 0; i < 16; i++) { h[i] ^= v[i] ^ v[i + 16]; }
  }

  // a += b + m[x]; d = rotr(d ^ a, 32); c += d; b = rotr(b ^ c, 24);
  // a += b + m[y]; d = rotr(d ^ a, 16); c += d; b = rotr(b ^ c, 63).
  // Each of a, b, c, d and the message words is a pair at an even index.
  function blake2bG(v, m, a, b, c, d, x, y) {
    let lo, hi, carry;
    // a = a + b + m[x]
    lo = v[a] + v[b]; carry = lo > 0xffffffff ? 1 : 0; hi = v[a + 1] + v[b + 1] + carry;
    lo = (lo >>> 0) + m[2 * x]; carry = lo > 0xffffffff ? 1 : 0; hi = hi + m[2 * x + 1] + carry;
    v[a] = lo >>> 0; v[a + 1] = hi >>> 0;
    // d = rotr(d ^ a, 32): the halves swap
    lo = v[d] ^ v[a]; hi = v[d + 1] ^ v[a + 1];
    v[d] = hi; v[d + 1] = lo;
    // c = c + d
    lo = v[c] + v[d]; carry = lo > 0xffffffff ? 1 : 0; hi = v[c + 1] + v[d + 1] + carry;
    v[c] = lo >>> 0; v[c + 1] = hi >>> 0;
    // b = rotr(b ^ c, 24)
    lo = v[b] ^ v[c]; hi = v[b + 1] ^ v[c + 1];
    v[b] = ((lo >>> 24) | (hi << 8)) >>> 0; v[b + 1] = ((hi >>> 24) | (lo << 8)) >>> 0;
    // a = a + b + m[y]
    lo = v[a] + v[b]; carry = lo > 0xffffffff ? 1 : 0; hi = v[a + 1] + v[b + 1] + carry;
    lo = (lo >>> 0) + m[2 * y]; carry = lo > 0xffffffff ? 1 : 0; hi = hi + m[2 * y + 1] + carry;
    v[a] = lo >>> 0; v[a + 1] = hi >>> 0;
    // d = rotr(d ^ a, 16)
    lo = v[d] ^ v[a]; hi = v[d + 1] ^ v[a + 1];
    v[d] = ((lo >>> 16) | (hi << 16)) >>> 0; v[d + 1] = ((hi >>> 16) | (lo << 16)) >>> 0;
    // c = c + d
    lo = v[c] + v[d]; carry = lo > 0xffffffff ? 1 : 0; hi = v[c + 1] + v[d + 1] + carry;
    v[c] = lo >>> 0; v[c + 1] = hi >>> 0;
    // b = rotr(b ^ c, 63) = rotl 1
    lo = v[b] ^ v[c]; hi = v[b + 1] ^ v[c + 1];
    v[b] = ((lo << 1) | (hi >>> 31)) >>> 0; v[b + 1] = ((hi << 1) | (lo >>> 31)) >>> 0;
  }

  // BLAKE2b of the bytes, outLength octets (1 to 64), unkeyed. Argon2 needs
  // digests of every length up to 64 and a fast path for 1024-byte inputs.
  function blake2b(input, outLength) {
    const h = new Uint32Array(BLAKE2B_IV);
    h[0] ^= 0x01010000 ^ outLength;
    const v = new Uint32Array(32);
    const m = new Uint32Array(32);
    const length = input.length;
    let t0 = 0, t1 = 0;
    let at = 0;
    // Every full block but the last; the last block, full or not, is
    // compressed with the final flag and the padding of zeros.
    while (length - at > 128) {
      for (let i = 0; i < 32; i++) { m[i] = input[at + 4 * i] | (input[at + 4 * i + 1] << 8) | (input[at + 4 * i + 2] << 16) | (input[at + 4 * i + 3] << 24); }
      at += 128;
      t0 = (t0 + 128) >>> 0; if (t0 < 128) { t1++; }
      blake2bCompress(h, m, v, t0, t1, false);
    }
    const rest = length - at;
    m.fill(0);
    for (let i = 0; i < rest; i++) { m[i >> 2] |= input[at + i] << (8 * (i & 3)); }
    t0 = (t0 + rest) >>> 0; if (t0 < rest) { t1++; }
    blake2bCompress(h, m, v, t0, t1, true);
    const out = new Uint8Array(outLength);
    for (let i = 0; i < outLength; i++) { out[i] = (h[i >> 2] >>> (8 * (i & 3))) & 0xff; }
    return out;
  }

  function le32(value) { return new Uint8Array([value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff]); }

  // H' of RFC 9106 section 3.3: BLAKE2b with the length prefixed for up to
  // 64 octets, and the chained construction beyond.
  function argon2Hash(outLength, input) {
    const prefixed = concat([le32(outLength), input]);
    if (outLength <= 64) { return blake2b(prefixed, outLength); }
    const r = Math.ceil(outLength / 32) - 2;
    const out = new Uint8Array(outLength);
    let v = blake2b(prefixed, 64);
    out.set(v.subarray(0, 32), 0);
    for (let i = 1; i < r; i++) {
      v = blake2b(v, 64);
      out.set(v.subarray(0, 32), 32 * i);
    }
    const lastLength = outLength - 32 * r;
    out.set(blake2b(v, lastLength), 32 * r);
    return out;
  }

  // The Argon2 mixing function GB on four words of a block (pairs at even
  // indices of the Uint32Array b): BLAKE2b's G with the multiplication
  // 2 * lo(a) * lo(b) added at each step, as section 3.6 defines it.
  function argon2GB(b, a, bb, c, d) {
    let lo, hi, carry, al, ah, bl, bh, ll, lh, hl, hh, plo, phi;
    // a = a + b + 2 * lo(a) * lo(b)
    al = b[a] & 0xffff; ah = b[a] >>> 16; bl = b[bb] & 0xffff; bh = b[bb] >>> 16;
    ll = al * bl; lh = al * bh; hl = ah * bl; hh = ah * bh;
    plo = ll + ((lh & 0xffff) * 65536) + ((hl & 0xffff) * 65536);
    phi = hh + (lh >>> 16) + (hl >>> 16) + Math.floor(plo / 4294967296);
    plo = plo >>> 0;
    // times two
    phi = ((phi << 1) | (plo >>> 31)) >>> 0; plo = (plo << 1) >>> 0;
    lo = b[a] + b[bb]; carry = lo > 0xffffffff ? 1 : 0; hi = b[a + 1] + b[bb + 1] + carry;
    lo = (lo >>> 0) + plo; carry = lo > 0xffffffff ? 1 : 0; hi = hi + phi + carry;
    b[a] = lo >>> 0; b[a + 1] = hi >>> 0;
    // d = rotr(d ^ a, 32)
    lo = b[d] ^ b[a]; hi = b[d + 1] ^ b[a + 1];
    b[d] = hi; b[d + 1] = lo;
    // c = c + d + 2 * lo(c) * lo(d)
    al = b[c] & 0xffff; ah = b[c] >>> 16; bl = b[d] & 0xffff; bh = b[d] >>> 16;
    ll = al * bl; lh = al * bh; hl = ah * bl; hh = ah * bh;
    plo = ll + ((lh & 0xffff) * 65536) + ((hl & 0xffff) * 65536);
    phi = hh + (lh >>> 16) + (hl >>> 16) + Math.floor(plo / 4294967296);
    plo = plo >>> 0;
    phi = ((phi << 1) | (plo >>> 31)) >>> 0; plo = (plo << 1) >>> 0;
    lo = b[c] + b[d]; carry = lo > 0xffffffff ? 1 : 0; hi = b[c + 1] + b[d + 1] + carry;
    lo = (lo >>> 0) + plo; carry = lo > 0xffffffff ? 1 : 0; hi = hi + phi + carry;
    b[c] = lo >>> 0; b[c + 1] = hi >>> 0;
    // b = rotr(b ^ c, 24)
    lo = b[bb] ^ b[c]; hi = b[bb + 1] ^ b[c + 1];
    b[bb] = ((lo >>> 24) | (hi << 8)) >>> 0; b[bb + 1] = ((hi >>> 24) | (lo << 8)) >>> 0;
    // a = a + b + 2 * lo(a) * lo(b)
    al = b[a] & 0xffff; ah = b[a] >>> 16; bl = b[bb] & 0xffff; bh = b[bb] >>> 16;
    ll = al * bl; lh = al * bh; hl = ah * bl; hh = ah * bh;
    plo = ll + ((lh & 0xffff) * 65536) + ((hl & 0xffff) * 65536);
    phi = hh + (lh >>> 16) + (hl >>> 16) + Math.floor(plo / 4294967296);
    plo = plo >>> 0;
    phi = ((phi << 1) | (plo >>> 31)) >>> 0; plo = (plo << 1) >>> 0;
    lo = b[a] + b[bb]; carry = lo > 0xffffffff ? 1 : 0; hi = b[a + 1] + b[bb + 1] + carry;
    lo = (lo >>> 0) + plo; carry = lo > 0xffffffff ? 1 : 0; hi = hi + phi + carry;
    b[a] = lo >>> 0; b[a + 1] = hi >>> 0;
    // d = rotr(d ^ a, 16)
    lo = b[d] ^ b[a]; hi = b[d + 1] ^ b[a + 1];
    b[d] = ((lo >>> 16) | (hi << 16)) >>> 0; b[d + 1] = ((hi >>> 16) | (lo << 16)) >>> 0;
    // c = c + d + 2 * lo(c) * lo(d)
    al = b[c] & 0xffff; ah = b[c] >>> 16; bl = b[d] & 0xffff; bh = b[d] >>> 16;
    ll = al * bl; lh = al * bh; hl = ah * bl; hh = ah * bh;
    plo = ll + ((lh & 0xffff) * 65536) + ((hl & 0xffff) * 65536);
    phi = hh + (lh >>> 16) + (hl >>> 16) + Math.floor(plo / 4294967296);
    plo = plo >>> 0;
    phi = ((phi << 1) | (plo >>> 31)) >>> 0; plo = (plo << 1) >>> 0;
    lo = b[c] + b[d]; carry = lo > 0xffffffff ? 1 : 0; hi = b[c + 1] + b[d + 1] + carry;
    lo = (lo >>> 0) + plo; carry = lo > 0xffffffff ? 1 : 0; hi = hi + phi + carry;
    b[c] = lo >>> 0; b[c + 1] = hi >>> 0;
    // b = rotr(b ^ c, 63)
    lo = b[bb] ^ b[c]; hi = b[bb + 1] ^ b[c + 1];
    b[bb] = ((lo << 1) | (hi >>> 31)) >>> 0; b[bb + 1] = ((hi << 1) | (lo >>> 31)) >>> 0;
  }

  // The permutation P of section 3.6 over sixteen words of a block, at the
  // pair indices given (the rows are contiguous, the columns strided).
  function argon2P(b, i0, i1, i2, i3, i4, i5, i6, i7, i8, i9, i10, i11, i12, i13, i14, i15) {
    argon2GB(b, i0, i4, i8, i12);
    argon2GB(b, i1, i5, i9, i13);
    argon2GB(b, i2, i6, i10, i14);
    argon2GB(b, i3, i7, i11, i15);
    argon2GB(b, i0, i5, i10, i15);
    argon2GB(b, i1, i6, i11, i12);
    argon2GB(b, i2, i7, i8, i13);
    argon2GB(b, i3, i4, i9, i14);
  }

  // The compression function G of section 3.5: memory is one Uint32Array of
  // every block (256 words each); prev, ref and next are block offsets in
  // words; withXor is the second and later passes' XOR into the block that
  // is there. r and tmp are two scratch blocks the caller owns.
  function argon2FillBlock(memory, prev, ref, next, withXor, r, tmp) {
    for (let i = 0; i < 256; i++) { r[i] = memory[prev + i] ^ memory[ref + i]; }
    if (withXor) { for (let i = 0; i < 256; i++) { tmp[i] = r[i] ^ memory[next + i]; } }
    else { tmp.set(r); }
    // Rows: eight rows of sixteen words (pair index 32 apart).
    for (let row = 0; row < 8; row++) {
      const o = row * 32;
      argon2P(r, o, o + 2, o + 4, o + 6, o + 8, o + 10, o + 12, o + 14, o + 16, o + 18, o + 20, o + 22, o + 24, o + 26, o + 28, o + 30);
    }
    // Columns: eight columns, the words two apart in each row.
    for (let col = 0; col < 8; col++) {
      const o = col * 4;
      argon2P(r, o, o + 2, o + 32, o + 34, o + 64, o + 66, o + 96, o + 98, o + 128, o + 130, o + 160, o + 162, o + 192, o + 194, o + 224, o + 226);
    }
    for (let i = 0; i < 256; i++) { memory[next + i] = tmp[i] ^ r[i]; }
  }

  const ARGON2_D = 0, ARGON2_I = 1, ARGON2_ID = 2;

  // Argon2 of RFC 9106: the three variants, version 0x13. spec: password and
  // salt as bytes, passes (t), memory (m, in KiB), lanes (p), length (T),
  // type (ARGON2_ID unless given), secret and ad (optional bytes). Answers a
  // promise so that a long derivation can yield to the page between slices
  // - a progress callback, when given, is told the pass and slice - and so
  // that a browser does not paint a frozen page for the seconds it takes.
  async function argon2(spec) {
    const password = anyToBytes(spec.password);
    const salt = anyToBytes(spec.salt);
    const passes = spec.passes;
    const memoryKiB = spec.memory;
    const lanes = spec.lanes;
    const outLength = spec.length || 32;
    const type = spec.type === undefined ? ARGON2_ID : spec.type;
    const secret = spec.secret ? anyToBytes(spec.secret) : new Uint8Array(0);
    const ad = spec.ad ? anyToBytes(spec.ad) : new Uint8Array(0);
    if (!(passes >= 1) || !(lanes >= 1 && lanes <= 0xffffff) || !(memoryKiB >= 8 * lanes) || !(outLength >= 4)) { throw new Error('Argon2 parameters out of range'); }

    // H0, and from it the first two blocks of every lane.
    const h0 = blake2b(concat([le32(lanes), le32(outLength), le32(memoryKiB), le32(passes), le32(0x13), le32(type),
      le32(password.length), password, le32(salt.length), salt, le32(secret.length), secret, le32(ad.length), ad]), 64);
    const blocks = 4 * lanes * Math.floor(memoryKiB / (4 * lanes));
    const laneLength = blocks / lanes;
    const segmentLength = laneLength / 4;
    const memory = new Uint32Array(blocks * 256);
    const putBlock = function (index, bytes) {
      const o = index * 256;
      for (let i = 0; i < 256; i++) { memory[o + i] = bytes[4 * i] | (bytes[4 * i + 1] << 8) | (bytes[4 * i + 2] << 16) | (bytes[4 * i + 3] << 24); }
    };
    for (let lane = 0; lane < lanes; lane++) {
      putBlock(lane * laneLength, argon2Hash(1024, concat([h0, le32(0), le32(lane)])));
      putBlock(lane * laneLength + 1, argon2Hash(1024, concat([h0, le32(1), le32(lane)])));
    }

    const r = new Uint32Array(256);
    const tmp = new Uint32Array(256);
    // For data-independent addressing: the input block, the address block,
    // and a zero block, as section 3.4.1.1 has them; kept in the memory
    // array's layout so that argon2FillBlock can be reused on them.
    const scratch = new Uint32Array(3 * 256);
    const inputAt = 0, addressAt = 256, zeroAt = 512;
    const nextAddresses = function () {
      // input[6]++, a 64-bit counter at pair 12
      scratch[inputAt + 12] = (scratch[inputAt + 12] + 1) >>> 0;
      if (scratch[inputAt + 12] === 0) { scratch[inputAt + 13] = (scratch[inputAt + 13] + 1) >>> 0; }
      argon2FillBlock(scratch, zeroAt, inputAt, addressAt, false, r, tmp);
      argon2FillBlock(scratch, zeroAt, addressAt, addressAt, false, r, tmp);
    };

    for (let pass = 0; pass < passes; pass++) {
      for (let slice = 0; slice < 4; slice++) {
        for (let lane = 0; lane < lanes; lane++) {
          const independent = type === ARGON2_I || (type === ARGON2_ID && pass === 0 && slice < 2);
          if (independent) {
            scratch.fill(0);
            scratch[inputAt + 0] = pass; scratch[inputAt + 2] = lane; scratch[inputAt + 4] = slice;
            scratch[inputAt + 6] = blocks; scratch[inputAt + 8] = passes; scratch[inputAt + 10] = type;
          }
          let start = 0;
          if (pass === 0 && slice === 0) {
            start = 2;
            if (independent) { nextAddresses(); }
          }
          let current = lane * laneLength + slice * segmentLength + start;
          let previous = current % laneLength === 0 ? current + laneLength - 1 : current - 1;
          for (let i = start; i < segmentLength; i++, current++, previous++) {
            if (current % laneLength === 1) { previous = current - 1; }
            let j1, j2;
            if (independent) {
              if (i % 128 === 0) { nextAddresses(); }
              j1 = scratch[addressAt + 2 * (i % 128)];
              j2 = scratch[addressAt + 2 * (i % 128) + 1];
            }
            else {
              j1 = memory[previous * 256];
              j2 = memory[previous * 256 + 1];
            }
            let refLane = j2 % lanes;
            if (pass === 0 && slice === 0) { refLane = lane; }
            const sameLane = refLane === lane;
            // The reference area of section 3.4.1.3, and the position in it.
            let areaSize;
            if (pass === 0) {
              if (slice === 0) { areaSize = i - 1; }
              else if (sameLane) { areaSize = slice * segmentLength + i - 1; }
              else { areaSize = slice * segmentLength + (i === 0 ? -1 : 0); }
            }
            else if (sameLane) { areaSize = laneLength - segmentLength + i - 1; }
            else { areaSize = laneLength - segmentLength + (i === 0 ? -1 : 0); }
            // relative = areaSize - 1 - floor(areaSize * floor(j1^2 / 2^32) / 2^32).
            // The high word of j1^2 EXACTLY: the square of a 32-bit word needs
            // 64 bits and a double carries 53, so (j1 * j1) / 2^32 in floating
            // point is one too large whenever the exact square's low word lies
            // within 2^10 of 2^32 - about once in four million fills, which at
            // 64 MiB and three passes is one derivation in fifty not being
            // Argon2id, and a mailbox key that opens on one machine and not,
            // some day, on another. The 16-bit split argon2GB uses keeps every
            // partial product exact; areaSize * x is under 2^50 and exact too.
            const j1lo = j1 & 0xffff, j1hi = j1 >>> 16;
            const x = j1hi * j1hi + Math.floor((2 * j1hi * j1lo * 65536 + j1lo * j1lo) / 4294967296);
            const y = Math.floor((areaSize * x) / 4294967296);
            const relative = areaSize - 1 - y;
            const startPosition = pass === 0 ? 0 : (slice === 3 ? 0 : (slice + 1) * segmentLength);
            const refIndex = (startPosition + relative) % laneLength;
            argon2FillBlock(memory, previous * 256, (refLane * laneLength + refIndex) * 256, current * 256, pass > 0, r, tmp);
          }
        }
        if (spec.progress) { spec.progress(pass, slice, passes); }
        if (spec.yield !== false) { await new Promise(function (resolve) { setTimeout(resolve, 0); }); }
      }
    }

    // The last column XORed across the lanes, then H' of it.
    const final = new Uint32Array(256);
    for (let lane = 0; lane < lanes; lane++) {
      const o = (lane * laneLength + laneLength - 1) * 256;
      for (let i = 0; i < 256; i++) { final[i] ^= memory[o + i]; }
    }
    const finalBytes = new Uint8Array(1024);
    for (let i = 0; i < 256; i++) { finalBytes[4 * i] = final[i] & 0xff; finalBytes[4 * i + 1] = (final[i] >>> 8) & 0xff; finalBytes[4 * i + 2] = (final[i] >>> 16) & 0xff; finalBytes[4 * i + 3] = (final[i] >>> 24) & 0xff; }
    memory.fill(0);
    return argon2Hash(outLength, finalBytes);
  }

  // The parameters a zero-access mailbox derives with: RFC 9106 section 4's
  // second recommendation, three passes over 64 MiB in four lanes. The page
  // may store lower memory for a device that cannot afford it; whatever the
  // row holds is what is derived with from then on.
  const ARGON2_DEFAULT = { passes: 3, memory: 65536, lanes: 4 };

  // The passphrase the mailbox key is sealed under, from the account
  // password: Argon2id over the password with the row's salt and
  // parameters, 32 octets, as 64 hex characters - a passphrase GnuPG would
  // accept as any other, which is what keeps the sealed copy an ordinary
  // OpenPGP secret key.
  async function zeroAccessPassphrase(password, saltBase64, params, progress) {
    const derived = await argon2({ password: utf8(String(password)), salt: b64decode(saltBase64), passes: params.passes, memory: params.memory, lanes: params.lanes, length: 32, type: ARGON2_ID, progress: progress });
    return hex(derived);
  }

  // The recovery key: 30 random octets, shown as eight groups of six base32
  // characters (RFC 4648's alphabet, 240 bits), which is what the person
  // writes down; and its canonical form - upper case, no separators - which
  // is the passphrase the recovery copy is sealed under, so a key typed with
  // or without the dashes, in either case, opens it.
  const BASE32_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
  function base32Encode(bytes) {
    let bits = 0, value = 0, out = '';
    for (let i = 0; i < bytes.length; i++) {
      value = ((value << 8) | bytes[i]) & 0x1fff;
      bits += 8;
      while (bits >= 5) { out += BASE32_ALPHABET[(value >>> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) { out += BASE32_ALPHABET[(value << (5 - bits)) & 31]; }
    return out;
  }
  function makeRecoveryKey() {
    const text = base32Encode(randomBytes(30));
    return text.match(/.{6}/g).join('-');
  }
  function canonicalRecoveryKey(typed) {
    const clean = String(typed || '').toUpperCase().replace(/[^A-Z2-7]/g, '');
    return clean.length === 48 ? clean : '';
  }

  // ---- what the page uses --------------------------------------------------

  return {
    // Whether the browser gave this page its cryptography at all - crypto is
    // withheld from a page that was not opened over HTTPS - and whether it has
    // the two Curve25519 algorithms, which is asked separately because RSA
    // still works where they are missing.
    available: !!subtle,
    curve25519Available: curve25519Available,

    // bytes and armour
    utf8: utf8, concat: concat, hex: hex, fromHex: fromHex, b64encode: b64encode, b64decode: b64decode,
    binaryString: binaryString, latin1Bytes: latin1Bytes, armor: armor, dearmor: dearmor, crc24: crc24, canonical: canonical,

    // packets, for the harness and for anything later that needs them
    readPackets: readPackets, writePacket: writePacket, TAG: TAG, HASH: HASH, SYM: SYM, ALGO: ALGO, FLAG: FLAG,

    // keys
    readKey: readKey, readKeys: readKeys, unlockKey: unlockKey, generateKey: generateKey,
    exportPublic: exportPublic, exportSecret: exportSecret, publicBytes: publicBytes, minimalPublicBytes: minimalPublicBytes,
    fingerprintText: fingerprintText, keyExpired: keyExpired, keyUsable: keyUsable, keyProblem: keyProblem,
    signingKeyPacket: signingKeyPacket, encryptionKeyPacket: encryptionKeyPacket,
    revocationCertificate: revocationCertificate, applyRevocation: applyRevocation, revocationReasonOf: revocationReasonOf,
    addressOfUserId: addressOfUserId,

    // the pieces, and the password-protected message built on them
    encryptBytes: encryptBytes, decryptBytes: decryptBytes, signDetached: signDetached, verifyDetached: verifyDetached,
    writeSkesk: writeSkesk, readSkesk: readSkesk, s2kDerive: s2kDerive,
    protectedMessage: protectedMessage, openProtected: openProtected,

    // messages
    readMessage: readMessage, composeMessage: composeMessage, looksLikeOpenPgp: looksLikeOpenPgp,

    // Autocrypt
    autocryptHeader: autocryptHeader, parseAutocrypt: parseAutocrypt,

    // BLAKE2b and Argon2 (RFC 7693, RFC 9106), and the two derivations a
    // zero-access mailbox stands on (docs/ZeroAccess.md)
    blake2b: blake2b, argon2: argon2, ARGON2: { D: ARGON2_D, I: ARGON2_I, ID: ARGON2_ID }, ARGON2_DEFAULT: ARGON2_DEFAULT,
    zeroAccessPassphrase: zeroAccessPassphrase, makeRecoveryKey: makeRecoveryKey, canonicalRecoveryKey: canonicalRecoveryKey, base32Encode: base32Encode
  };
}));

