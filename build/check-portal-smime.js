// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Runs the page's S/MIME module (PortalSmime.js) in Node against fixtures
// OpenSSL made - certificates, keys in three forms, PKCS#12 files with right
// and wrong passwords, messages signed detached, opaque and by an EC key,
// messages encrypted with AES-256 and AES-128, one signed then encrypted, one
// tampered with - and hands what the module signs and encrypts to OpenSSL to
// verify and decrypt, so both directions are checked against an
// implementation that is not this one.
//
// Usage: node build/check-portal-smime.js [module.js] [fixtures dir]
// Needs Node 20 or later (Web Crypto) and openssl on the PATH.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const nodeCrypto = require('crypto');
const { execFileSync, spawnSync } = require('child_process');

const modulePath = process.argv[2] || path.join(__dirname, '..', 'hmailserver', 'source', 'Server', 'Common', 'Util', 'PortalSmime.js');
const smime = require(path.resolve(modulePath));
// The fixtures are made fresh by OpenSSL (build/make-smime-fixtures.sh) unless a
// directory holding them is given - they hold private keys, so none is committed.
let fixtures = process.argv[3] || '';
let madeFixtures = false;
if (!fixtures) {
  fixtures = fs.mkdtempSync(path.join(os.tmpdir(), 'hm-smime-fixtures-'));
  execFileSync('bash', [path.join(__dirname, 'make-smime-fixtures.sh'), fixtures], { stdio: ['ignore', 'ignore', 'inherit'] });
  madeFixtures = true;
}

let failures = 0;
let checks = 0;
function check(condition, what) {
  checks++;
  if (condition) { console.log('  ok   ' + what); } else { failures++; console.log('  FAIL ' + what); }
}
function read(name) { return new Uint8Array(fs.readFileSync(path.join(fixtures, name))); }
function text(bytes) { return Buffer.from(bytes).toString('utf8'); }
const PASSWORD = 'fixture-pass';
// body.txt is written with LF endings; what OpenSSL signed and encrypted is its canonical CRLF form.
const BODY_CONTENT = 'A signed message from the fixtures.\r\nSecond line, with caf\u00e9.\r\n';

// Everything OpenSSL said, standard output and standard error together, and
// whatever its exit status.
function verdictOf(args) {
  const run = spawnSync('openssl', args, { stdio: ['ignore', 'pipe', 'pipe'] });
  return (run.stdout ? run.stdout.toString('utf8') : '') + (run.stderr ? run.stderr.toString('utf8') : '') + (run.error ? String(run.error) : '');
}

function openssl(args, input) {
  try {
    return { ok: true, out: execFileSync('openssl', args, { input: input, stdio: ['pipe', 'pipe', 'pipe'] }).toString('utf8') };
  } catch (e) {
    return { ok: false, out: (e.stdout ? e.stdout.toString() : '') + (e.stderr ? e.stderr.toString() : '') };
  }
}

// What a wrong password hands back about one time in 255: AES-CBC's padding
// check passes when the last block happens to decrypt to a valid padding, and
// the rest is noise. Made certain: the PBES2 ciphertext of an OpenSSL-made
// file, rewritten in place to the noise given (one byte shorter, so that the
// padding is one byte and the length stays) under the key the wrong password
// derives with the file's own salt, iteration count and IV. Answers the bytes
// the wrong password decrypts to, by Node's own AES-CBC.
const PRF_BY_OID = { '1.2.840.113549.2.7': 'sha1', '1.2.840.113549.2.9': 'sha256', '1.2.840.113549.2.10': 'sha384', '1.2.840.113549.2.11': 'sha512' };
const CIPHER_BY_OID = { '2.16.840.1.101.3.4.1.2': 'aes-128-cbc', '2.16.840.1.101.3.4.1.22': 'aes-192-cbc', '2.16.840.1.101.3.4.1.42': 'aes-256-cbc' };
function forgePbes2(algorithmNode, ciphertextNode, password, noise) {
  if (ciphertextNode.constructed || noise.length !== ciphertextNode.content.length - 1) { throw new Error('the ciphertext cannot be rewritten in place'); }
  const params = smime.derChildren(smime.derChildren(algorithmNode)[1]);
  const kdf = smime.derChildren(smime.derChildren(params[0])[1]);
  const scheme = smime.derChildren(params[1]);
  const cipher = CIPHER_BY_OID[smime.oidDecode(scheme[0].content)];
  let prf = 'sha1';
  for (let i = 2; i < kdf.length; i++) { if (kdf[i].tag === 0x30) { prf = PRF_BY_OID[smime.oidDecode(smime.derChildren(kdf[i])[0].content)]; } }
  if (!cipher || !prf) { throw new Error('the file is encrypted with something other than PBKDF2 and AES-CBC'); }
  const iterations = Number(BigInt('0x' + Buffer.from(kdf[1].content).toString('hex')));
  const key = nodeCrypto.pbkdf2Sync(Buffer.from(password, 'utf8'), Buffer.from(kdf[0].content), iterations, Number(cipher.slice(4, 7)) / 8, prf);
  const iv = Buffer.from(scheme[1].content);
  const encryptor = nodeCrypto.createCipheriv(cipher, key, iv);
  ciphertextNode.content.set(Buffer.concat([encryptor.update(noise), encryptor.final()]));
  const decryptor = nodeCrypto.createDecipheriv(cipher, key, iv);
  return Buffer.concat([decryptor.update(Buffer.from(ciphertextNode.content)), decryptor.final()]);
}

function derHeader(tag, length) {
  if (length < 0x80) { return Buffer.from([tag, length]); }
  const bytes = [];
  for (let v = length; v > 0; v = Math.floor(v / 256)) { bytes.unshift(v & 0xff); }
  return Buffer.from([tag, 0x80 | bytes.length].concat(bytes));
}

// Three kinds of noise, each of the length given: one whole DER element and
// then anything, which 6.3.6 read as a key because one element read (and as a
// safe, as it read anything); a SEQUENCE that ends before the bytes do; and a
// SEQUENCE over all of them that holds no key, only an OCTET STRING of zeros.
function noiseLeadingElement(length) { return Buffer.concat([Buffer.from([0x04, 0x01, 0x00]), nodeCrypto.randomBytes(length - 3)]); }
function noiseShortSequence(length) { return Buffer.concat([Buffer.from([0x30, 0x03, 0x02, 0x01, 0x00]), nodeCrypto.randomBytes(length - 5)]); }
function noiseWholeSequence(length) {
  for (let n = length; n >= 0; n--) {
    const inner = Buffer.concat([derHeader(0x04, n), Buffer.alloc(n)]);
    const outer = Buffer.concat([derHeader(0x30, inner.length), inner]);
    if (outer.length === length) { return outer; }
  }
  throw new Error('no SEQUENCE is ' + length + ' bytes long');
}

// The PBES2 pieces of an OpenSSL-made PKCS#12, as [algorithm, ciphertext]
// nodes over the file's own bytes: its first encrypted safe's, and the first
// shrouded key's that lies in a safe that is not encrypted.
function pkcs12Pieces(bytes) {
  const authSafe = smime.derChildren(smime.derChildren(smime.derChildren(smime.derRead(bytes))[1])[1])[0];
  if (authSafe.constructed) { throw new Error('the AuthenticatedSafe is in pieces'); }
  const pieces = { safe: null, key: null };
  for (const info of smime.derChildren(smime.derRead(authSafe.content))) {
    const kids = smime.derChildren(info);
    const inner = smime.derChildren(kids[1])[0];
    const type = smime.oidDecode(kids[0].content);
    if (type === smime.OID.encryptedData && !pieces.safe) {
      const eci = smime.derChildren(smime.derChildren(inner)[1]);
      pieces.safe = [eci[1], eci[2]];
    } else if (type === smime.OID.data && !inner.constructed) {
      for (const bag of smime.derChildren(smime.derRead(inner.content))) {
        const bagKids = smime.derChildren(bag);
        if (smime.oidDecode(bagKids[0].content) === smime.OID.shroudedKeyBag && !pieces.key) {
          const encrypted = smime.derChildren(smime.derChildren(bagKids[1])[0]);
          pieces.key = [encrypted[0], encrypted[1]];
        }
      }
    }
  }
  return pieces;
}

// A PKCS#12's MAC made again, by Node's own hashes and not the module's: RFC
// 7292 appendix B.2 with the MAC's ID, 3 - the hash, iterated, of a block of
// 3s, then the salt and the password's bytes, each repeated to whole blocks (64
// bytes, or 128 for SHA-384 and SHA-512) - gives the HMAC key, and the HMAC is
// of the AuthenticatedSafe's octets. The password's bytes are given as the MAC
// takes them: the BMPString, two zero bytes on the end; or no bytes at all; or
// the UTF-8 bytes each widened to two, as OpenSSL wrote them before 1.1.0.
const MAC_HASHES = { '1.3.14.3.2.26': ['sha1', 64], '2.16.840.1.101.3.4.2.1': ['sha256', 64], '2.16.840.1.101.3.4.2.2': ['sha384', 128], '2.16.840.1.101.3.4.2.3': ['sha512', 128] };
function bmpPassword(password) {
  const out = Buffer.alloc(password.length * 2 + 2);
  for (let i = 0; i < password.length; i++) { out.writeUInt16BE(password.charCodeAt(i), i * 2); }
  return out;
}
function widenedPassword(password) {
  const bytes = Buffer.from(password, 'utf8');
  const out = Buffer.alloc(bytes.length * 2 + 2);
  for (let i = 0; i < bytes.length; i++) { out[i * 2 + 1] = bytes[i]; }
  return out;
}
function macParts(bytes) {
  const pfx = smime.derChildren(smime.derRead(bytes));
  const content = smime.derChildren(smime.derChildren(pfx[1])[1])[0];
  if (pfx.length !== 3 || content.constructed) { throw new Error('not a PKCS#12 with a MAC and its AuthenticatedSafe in one piece'); }
  const macData = smime.derChildren(pfx[2]);
  const digestInfo = smime.derChildren(macData[0]);
  return {
    version: Buffer.from(pfx[0].raw), authSafe: Buffer.from(pfx[1].raw), content: Buffer.from(content.content),
    algorithm: Buffer.from(digestInfo[0].raw), hashOid: smime.oidDecode(smime.derChildren(digestInfo[0])[0].content), digest: Buffer.from(digestInfo[1].content),
    salt: Buffer.from(macData[1].content), iterations: macData.length > 2 ? Number(BigInt('0x' + Buffer.from(macData[2].content).toString('hex'))) : 1
  };
}
function macOf(parts, passwordBytes, iterations) {
  const [hash, blockSize] = MAC_HASHES[parts.hashOid];
  const fill = function (part) {
    const out = Buffer.alloc(part.length ? blockSize * Math.ceil(part.length / blockSize) : 0);
    for (let i = 0; i < out.length; i++) { out[i] = part[i % part.length]; }
    return out;
  };
  let key = nodeCrypto.createHash(hash).update(Buffer.alloc(blockSize, 3)).update(fill(parts.salt)).update(fill(passwordBytes)).digest();
  for (let i = 1; i < iterations; i++) { key = nodeCrypto.createHash(hash).update(key).digest(); }
  return nodeCrypto.createHmac(hash, key).update(parts.content).digest();
}
function der(tag, ...contents) { const content = Buffer.concat(contents); return Buffer.concat([derHeader(tag, content.length), content]); }
function derInteger(value) {
  let digits = value.toString(16);
  if (digits.length % 2) { digits = '0' + digits; }
  const bytes = Buffer.from(digits, 'hex');
  return der(0x02, bytes[0] & 0x80 ? Buffer.concat([Buffer.from([0]), bytes]) : bytes);
}
// The file with its MacData written again: the MAC made over the password's
// bytes given, or the file's own MAC kept, under the iteration count given or
// the file's own.
function withMac(bytes, change) {
  const parts = macParts(bytes);
  const iterations = change.iterations === undefined ? parts.iterations : change.iterations;
  const digest = change.passwordBytes ? macOf(parts, change.passwordBytes, iterations) : parts.digest;
  const macData = der(0x30, der(0x30, parts.algorithm, der(0x04, digest)), der(0x04, parts.salt), derInteger(iterations));
  return new Uint8Array(der(0x30, parts.version, parts.authSafe, macData));
}

function pem(label, der) { return Buffer.from('-----BEGIN ' + label + '-----\n' + Buffer.from(der).toString('base64').replace(/(.{64})/g, '$1\n').replace(/\n$/, '') + '\n-----END ' + label + '-----\n'); }

async function messageOf(promise) { try { await promise; return 'IMPORTED'; } catch (e) { return e.message; } }
// "IMPORTED" when the file gave one key paired with its certificate; what it
// gave, or the error, otherwise.
async function importedAs(bytes, password) {
  try {
    const found = await smime.importAny(bytes, password);
    return found.keys.length === 1 && found.keys[0].certificate ? 'IMPORTED' : found.keys.length + ' key(s), ' + found.certificates.length + ' certificate(s)';
  } catch (e) {
    return e.message;
  }
}

(async function () {
  console.log('== certificates');
  const alice = smime.parseCertificate(read('alice.der'));
  const facts = text(read('alice.facts'));
  const serial = (facts.match(/serial=([0-9A-F]+)/i) || [])[1].toLowerCase();
  const fp = (facts.match(/Fingerprint=([0-9A-F:]+)/i) || [])[1].replace(/:/g, '').toLowerCase();
  check(alice.emails.indexOf('alice@example.test') >= 0, 'the leaf names alice@example.test (subject emailAddress and SAN): ' + alice.emails.join(', '));
  check(alice.serialHex === serial, 'the serial matches OpenSSL: ' + alice.serialHex);
  check((await smime.fingerprint(alice)) === fp, 'the SHA-256 fingerprint matches OpenSSL');
  check(alice.keyType === 'rsa' && alice.rsa.bytes === 256, 'an RSA-2048 key');
  check(alice.subject.map.CN === 'Alice Example' && alice.issuer.map.CN === 'Fixture CA', 'subject and issuer read: ' + alice.subject.text + ' / ' + alice.issuer.text);
  check(!alice.isCA && alice.keyUsage && alice.keyUsage.digitalSignature && alice.keyUsage.keyEncipherment, 'not a CA; signs and enciphers');
  check(alice.extendedKeyUsage.indexOf('1.3.6.1.5.5.7.3.4') >= 0, 'extended key usage emailProtection');
  check(alice.notAfter.getTime() > Date.now(), 'validity read: until ' + alice.notAfter.toISOString().slice(0, 10));
  const ca = smime.parseCertificate(read('ca.der'));
  check(ca.isCA, 'the CA is a CA');
  const erin = smime.parseCertificate(read('erin.der'));
  check(erin.keyType === 'ec' && erin.curve === 'P-256', 'the EC leaf is P-256');

  console.log('== keys');
  const clear = await smime.importAny(read('alice-key.pem'), '');
  check(clear.keys.length === 1 && clear.keys[0].key.type === 'rsa', 'PKCS#8 PEM imported');
  check(smime.keyMatchesCertificate(clear.keys[0].key, alice), 'the key matches the certificate');
  const encrypted = await smime.importAny(read('alice-key-encrypted.pem'), PASSWORD);
  check(encrypted.keys.length === 1 && smime.keyMatchesCertificate(encrypted.keys[0].key, alice), 'PBES2-encrypted PKCS#8 PEM imported with the password');
  let wrong = '';
  try { await smime.importAny(read('alice-key-encrypted.pem'), 'nope'); } catch (e) { wrong = e.message; }
  check(wrong === 'wrong password', 'a wrong password is named: ' + wrong);
  // The check above is a gamble about one time in 255: a wrong password's last
  // block then decrypts to a valid padding, and until 6.3.7 the noise in front
  // of it was read as a key whenever one DER element read, and named "DER: bad
  // length" or the like. Made certain here, for each kind of noise: the
  // encrypted key with its ciphertext rewritten so that 'nope' really does
  // decrypt it, padding and all.
  for (const [kind, makeNoise] of [['one DER element and then noise', noiseLeadingElement], ['a SEQUENCE that ends before the bytes do', noiseShortSequence],
    ['a SEQUENCE over all of it that holds no key', noiseWholeSequence]]) {
    const der = smime.pemBlocks(text(read('alice-key-encrypted.pem')))[0].bytes;
    const parts = smime.derChildren(smime.derRead(der));
    const noise = makeNoise(parts[1].content.length - 1);
    const decrypted = forgePbes2(parts[0], parts[1], 'nope', noise);
    const said = await messageOf(smime.importAny(new Uint8Array(pem('ENCRYPTED PRIVATE KEY', der)), 'nope'));
    check(Buffer.compare(decrypted, noise) === 0 && said === 'wrong password', 'a wrong password that the padding lets through, to ' + kind + ', is named: ' + said);
  }
  const p12 = await smime.importAny(read('alice.p12'), PASSWORD);
  check(p12.keys.length === 1 && p12.keys[0].certificate && p12.keys[0].certificate.emails[0] === 'alice@example.test', 'PKCS#12 imported: the key is paired with its certificate');
  check(p12.certificates.length === 2, 'PKCS#12 carries the chain: ' + p12.certificates.map(function (c) { return c.name; }).join(', '));
  // A wrong password for a PKCS#12 is told by its MAC, before any safe is
  // decrypted; until 6.3.7 it was the same gamble as the key's above.
  const p12Wrong = await messageOf(smime.importAny(read('alice.p12'), 'nope'));
  check(p12Wrong === 'wrong password', 'a wrong password for the PKCS#12 is named: ' + p12Wrong);
  // A file whose safes are not encrypted: only its MAC knows the password, and
  // until 6.3.7, which never read the MAC, any password imported it.
  const clearP12 = await smime.importAny(read('alice-clear.p12'), PASSWORD);
  check(clearP12.keys.length === 1 && !!clearP12.keys[0].certificate, 'a PKCS#12 whose safes are not encrypted imports with its password');
  const clearWrong = await messageOf(smime.importAny(read('alice-clear.p12'), 'nope'));
  check(clearWrong === 'wrong password', 'and its MAC names a wrong one: ' + clearWrong);
  // Files with no MAC: the padding, and then the shape of what each safe and
  // each key decrypts to, tell a wrong password - made certain as for the key
  // above, with an encrypted safe, and a key in a safe that is not, rewritten
  // so that 'nope' decrypts them.
  for (const [name, piece, what] of [['alice-nomac.p12', 'safe', 'an encrypted safe'], ['alice-nomac-plaincerts.p12', 'key', 'a shrouded key']]) {
    const imported = await smime.importAny(read(name), PASSWORD);
    check(imported.keys.length === 1 && !!imported.keys[0].certificate, 'a PKCS#12 with no MAC, ' + what + ' in it, imports with its password');
    const wrongOne = await messageOf(smime.importAny(read(name), 'nope'));
    check(wrongOne === 'wrong password', 'and a wrong one is named: ' + wrongOne);
    const bytes = read(name);
    const pieces = pkcs12Pieces(bytes)[piece];
    const noise = noiseLeadingElement(pieces[1].content.length - 1);
    const decrypted = forgePbes2(pieces[0], pieces[1], 'nope', noise);
    const said = await messageOf(smime.importAny(bytes, 'nope'));
    check(Buffer.compare(decrypted, noise) === 0 && said === 'wrong password', 'a wrong password that ' + what + '\'s padding lets through, to noise, is named: ' + said);
  }
  // The MAC takes the password as UTF-16 with two zero bytes on the end, and
  // PBKDF2 as UTF-8: a file made with no password, and one made with a
  // password that is not ASCII (an accented letter, a euro sign and an emoji).
  const emptyP12 = await smime.importAny(read('alice-empty.p12'), '');
  check(emptyP12.keys.length === 1 && !!emptyP12.keys[0].certificate, 'a PKCS#12 made with an empty password imports with none');
  const emptyWrong = await messageOf(smime.importAny(read('alice-empty.p12'), 'nope'));
  check(emptyWrong === 'wrong password', 'and a password given for it is named wrong: ' + emptyWrong);
  const unicodePassword = text(read('unicode-pass.txt'));
  const unicodeP12 = await smime.importAny(read('alice-unicode.p12'), unicodePassword);
  check(unicodeP12.keys.length === 1 && !!unicodeP12.keys[0].certificate, 'a PKCS#12 made with a password that is not ASCII imports with it');
  const unicodeWrong = await messageOf(smime.importAny(read('alice-unicode.p12'), unicodePassword.normalize('NFD')));
  check(unicodeWrong === 'wrong password', 'and the same password with its accent written apart is named wrong: ' + unicodeWrong);
  // The MAC's two other forms of a password, which no exporter at hand
  // writes - OpenSSL 3 makes an empty password's MAC over the BMPString's two
  // zero bytes, and one that is not ASCII over its BMPString - but which
  // OpenSSL's own import takes, as the page must: alice-clear.p12, in which
  // nothing is encrypted, with its MAC made again over that form alone, so
  // that only that form lets a password through. First, that the derivation
  // gives the MAC OpenSSL made.
  const clearParts = macParts(read('alice-clear.p12'));
  check(macOf(clearParts, bmpPassword(PASSWORD), clearParts.iterations).equals(clearParts.digest), 'the harness\'s own MAC (RFC 7292 B.2, by Node) is the one OpenSSL made for alice-clear.p12');
  const macTmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hm-smime-mac-'));
  const noBytes = withMac(read('alice-clear.p12'), { passwordBytes: Buffer.alloc(0) });
  fs.writeFileSync(path.join(macTmp, 'no-bytes.p12'), noBytes);
  const noBytesOpenssl = openssl(['pkcs12', '-in', path.join(macTmp, 'no-bytes.p12'), '-noout', '-passin', 'pass:']);
  const noBytesEmpty = await importedAs(noBytes, '');
  check(noBytesOpenssl.ok && noBytesEmpty === 'IMPORTED', 'a MAC made over no bytes, as for no password at all, which OpenSSL takes for an empty one, imports with an empty password: ' +
    noBytesEmpty + (noBytesOpenssl.ok ? '' : '; OpenSSL: ' + noBytesOpenssl.out.trim()));
  const noBytesWrong = await importedAs(noBytes, 'nope');
  check(noBytesWrong === 'wrong password', 'and names a wrong one: ' + noBytesWrong);
  const widened = withMac(read('alice-clear.p12'), { passwordBytes: widenedPassword(unicodePassword) });
  fs.writeFileSync(path.join(macTmp, 'widened.p12'), widened);
  const widenedOpenssl = openssl(['pkcs12', '-in', path.join(macTmp, 'widened.p12'), '-noout', '-passin', 'file:' + path.join(fixtures, 'unicode-pass.txt')]);
  const widenedRight = await importedAs(widened, unicodePassword);
  check(widenedOpenssl.ok && widenedRight === 'IMPORTED', 'a MAC made over the password\'s UTF-8 bytes each widened to two, as OpenSSL wrote one that is not ASCII before 1.1.0 and still takes it, imports with that password: ' +
    widenedRight + (widenedOpenssl.ok ? '' : '; OpenSSL: ' + widenedOpenssl.out.trim()));
  const widenedWrong = await importedAs(widened, unicodePassword.normalize('NFD'));
  check(widenedWrong === 'wrong password', 'and that password with its accent written apart is named wrong by the MAC too: ' + widenedWrong);
  // A MAC the page cannot compute is passed over, as a file with no MAC is:
  // with nothing encrypted, any password then imports the file, and the right
  // one must. A MAC by SHA-224, as OpenSSL's -macalg sha224 makes it; one whose
  // count is past the page's bound, as a damaged file's may be, which would
  // hold the import for hours if it ran; and, where OpenSSL makes one, RFC
  // 9579's PBMAC1, with its safes encrypted, so that they name a wrong password.
  const sha224 = read('alice-clear-sha224.p12');
  const sha224Right = await importedAs(sha224, PASSWORD);
  const sha224Other = await importedAs(sha224, 'nope');
  check(macParts(sha224).hashOid === '2.16.840.1.101.3.4.2.4' && sha224Right === 'IMPORTED' && sha224Other === 'IMPORTED',
    'a MAC by SHA-224, which Web Crypto lacks, is passed over: the file, nothing in it encrypted, imports with its password and with another: ' + sha224Right + ', ' + sha224Other);
  const pastBound = withMac(read('alice-clear.p12'), { iterations: 1000001 });
  const pastRight = await importedAs(pastBound, PASSWORD);
  const pastOther = await importedAs(pastBound, 'nope');
  check(pastRight === 'IMPORTED' && pastOther === 'IMPORTED', 'a MAC whose count is past 1,000,000 is passed over, not run: the file imports with its password and with another: ' + pastRight + ', ' + pastOther);
  if (fs.existsSync(path.join(fixtures, 'alice-pbmac1.p12'))) {
    const pbmac1 = read('alice-pbmac1.p12');
    const pbmac1Right = await importedAs(pbmac1, PASSWORD);
    const pbmac1Wrong = await importedAs(pbmac1, 'nope');
    check(macParts(pbmac1).hashOid === '1.2.840.113549.1.5.14' && pbmac1Right === 'IMPORTED' && pbmac1Wrong === 'wrong password',
      'a MAC by RFC 9579\'s PBMAC1, as OpenSSL 3.4 and later make it, is passed over: the file imports with its password, and its encrypted safes name a wrong one: ' + pbmac1Right + ', ' + pbmac1Wrong);
  }
  fs.rmSync(macTmp, { recursive: true, force: true });
  if (fs.existsSync(path.join(fixtures, 'alice-legacy.p12'))) {
    let legacy = '';
    try { await smime.importAny(read('alice-legacy.p12'), PASSWORD); } catch (e) { legacy = e.message; }
    check(/legacy/.test(legacy), 'a legacy (3DES/RC2) PKCS#12 is refused with advice: ' + legacy);
    // Its MAC knows the password, where 6.3.6 gave the advice for any.
    const legacyWrong = await importedAs(read('alice-legacy.p12'), 'nope');
    check(legacyWrong === 'wrong password', 'and a wrong password for it is named by its MAC: ' + legacyWrong);
  }
  const der8 = await smime.importAny(read('alice-key.p8'), '');
  check(der8.keys.length === 1 && smime.keyMatchesCertificate(der8.keys[0].key, alice), 'DER PKCS#8 imported');
  const pemCert = await smime.importAny(read('alice.pem'), '');
  check(pemCert.certificates.length === 1 && pemCert.keys.length === 0, 'a PEM certificate alone imports');
  const erinKey = (await smime.importAny(read('erin-key.pem'), '')).keys[0].key;
  check(erinKey.type === 'ec' && smime.keyMatchesCertificate(erinKey, erin), 'the EC key matches the EC certificate');

  console.log('== wrapping');
  const wrapped = await smime.wrapPrivateKey(clear.keys[0].key.pkcs8, 'account-password');
  const unwrapped = await smime.unwrapPrivateKey(wrapped, 'account-password');
  check(Buffer.compare(Buffer.from(unwrapped), Buffer.from(clear.keys[0].key.pkcs8)) === 0, 'a wrapped key unwraps to the same PKCS#8');
  let bad = '';
  try { await smime.unwrapPrivateKey(wrapped, 'other'); } catch (e) { bad = e.message; }
  check(bad === 'wrong password', 'unwrapping with another password fails: ' + bad);
  check(wrapped.v === 1 && wrapped.kdf === 'PBKDF2-SHA256' && wrapped.iterations === 600000 && wrapped.salt.length >= 20,
    'the envelope says how it was made: v' + wrapped.v + ', ' + wrapped.kdf + ' x ' + wrapped.iterations);
  // A row stored before the server kept the version marker: neither field.
  const older = { iterations: wrapped.iterations, salt: wrapped.salt, iv: wrapped.iv, data: wrapped.data };
  const fromOlder = await smime.unwrapPrivateKey(older, 'account-password');
  check(Buffer.compare(Buffer.from(fromOlder), Buffer.from(clear.keys[0].key.pkcs8)) === 0, 'an envelope with no version and no kdf is read as the only one this module has written');
  let refused = '';
  try { await smime.unwrapPrivateKey(Object.assign({}, wrapped, { kdf: 'scrypt' }), 'account-password'); } catch (e) { refused = e.message; }
  check(/scrypt/.test(refused) && refused !== 'wrong password', 'a derivation this module does not know is refused by name, not mis-derived: ' + refused);
  let refusedVersion = '';
  try { await smime.unwrapPrivateKey(Object.assign({}, wrapped, { v: 2 }), 'account-password'); } catch (e) { refusedVersion = e.message; }
  check(/version 2/.test(refusedVersion) && refusedVersion !== 'wrong password', 'a version this module does not know is refused by name: ' + refusedVersion);

  console.log('== verifying what OpenSSL signed');
  const own = [{ key: clear.keys[0].key, certificate: alice }];
  const detached = await smime.readMessage(read('signed-detached.eml'), own);
  check(detached.signed && detached.signed.valid, 'detached signature valid: ' + (detached.signed && detached.signed.error));
  check(detached.signed && detached.signed.signer && detached.signed.signer.emails[0] === 'alice@example.test', 'signed by alice');
  check(detached.signed && detached.signed.certificates.length === 2, 'the message carries the leaf and the CA');
  check(detached.content && detached.content.text === BODY_CONTENT, 'the text is the fixture body: ' + JSON.stringify(detached.content && detached.content.text.slice(0, 40)));
  check(detached.signed && detached.signed.signingTime instanceof Date, 'the signing time is read');
  const opaque = await smime.readMessage(read('signed-opaque.eml'), own);
  check(opaque.signed && opaque.signed.valid && opaque.content.text === BODY_CONTENT, 'opaque signature valid and its content read');
  const ec = await smime.readMessage(read('signed-ec.eml'), own);
  check(ec.signed && ec.signed.valid && ec.signed.signer.emails[0] === 'erin@example.test', 'an ECDSA P-256 signature verifies: ' + (ec.signed && ec.signed.error));
  const tampered = await smime.readMessage(read('tampered.eml'), own);
  check(tampered.signed && !tampered.signed.valid && /changed/.test(tampered.signed.error), 'a changed body fails: ' + (tampered.signed && tampered.signed.error));

  console.log('== decrypting what OpenSSL encrypted');
  const enc = await smime.readMessage(read('encrypted.eml'), own);
  check(enc.encrypted && enc.encrypted.ok && enc.content.text === BODY_CONTENT, 'AES-256-CBC + RSAES-PKCS1-v1_5 decrypted: ' + (enc.encrypted && enc.encrypted.error));
  const enc128 = await smime.readMessage(read('encrypted-aes128.eml'), own);
  check(enc128.encrypted && enc128.encrypted.ok && enc128.content.text === BODY_CONTENT, 'AES-128-CBC decrypted');
  const both = await smime.readMessage(read('signed-encrypted.eml'), own);
  check(both.encrypted && both.encrypted.ok && both.signed && both.signed.valid && both.content.text === BODY_CONTENT, 'signed then encrypted: decrypted, then the signature verified; layers ' + both.layers.join(' > '));
  const notMine = await smime.readMessage(read('encrypted.eml'), [{ key: erinKey, certificate: erin }]);
  check(notMine.encrypted && !notMine.encrypted.ok && /not encrypted for/.test(notMine.encrypted.error), 'another key cannot open it: ' + (notMine.encrypted && notMine.encrypted.error));

  console.log('== OpenSSL verifying and decrypting what the module made');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hm-smime-'));
  const signer = { key: clear.keys[0].key, certificate: alice, chain: [ca] };
  const signedText = await smime.composeMessage({ headers: { from: 'alice@example.test', to: 'bob@example.test', subject: 'Signed on the page, with café' }, text: 'Hello Bob,\r\n\r\nSigned in the browser.\r\n', html: '<p>Hello <b>Bob</b>,</p><p>Signed in the browser.</p>', attachments: [{ name: 'note.txt', type: 'text/plain', data: Buffer.from('an attachment').toString('base64') }], sign: signer });
  fs.writeFileSync(path.join(tmp, 'page-signed.eml'), signedText, 'binary');
  let r = openssl(['cms', '-verify', '-in', path.join(tmp, 'page-signed.eml'), '-CAfile', path.join(fixtures, 'ca.pem'), '-out', path.join(tmp, 'page-signed-content.txt')]);
  check(r.ok, 'openssl cms -verify accepts the page\'s detached signature' + (r.ok ? '' : ': ' + r.out.trim()));
  const verifiedContent = r.ok ? fs.readFileSync(path.join(tmp, 'page-signed-content.txt'), 'binary') : '';
  check(/Signed in the browser/.test(verifiedContent) && /multipart\/mixed/.test(verifiedContent) && /name="note.txt"/.test(verifiedContent), 'the signed content is the text, the HTML and the attachment');
  const back = await smime.readMessage(smime.latin1Bytes(signedText), own);
  check(back.signed && back.signed.valid && back.content.text === 'Hello Bob,\r\n\r\nSigned in the browser.\r\n' && back.content.attachments.length === 1 && text(back.content.attachments[0].bytes) === 'an attachment', 'the module reads its own signed message back');
  const ecSigned = await smime.composeMessage({ headers: { from: 'erin@example.test', to: 'bob@example.test', subject: 'EC' }, text: 'EC signed', sign: { key: erinKey, certificate: erin, chain: [ca] } });
  fs.writeFileSync(path.join(tmp, 'page-ec.eml'), ecSigned, 'binary');
  r = openssl(['cms', '-verify', '-in', path.join(tmp, 'page-ec.eml'), '-CAfile', path.join(fixtures, 'ca.pem'), '-out', path.join(tmp, 'page-ec-content.txt')]);
  check(r.ok, 'openssl cms -verify accepts the page\'s ECDSA signature' + (r.ok ? '' : ': ' + r.out.trim()));

  const encryptedText = await smime.composeMessage({ headers: { from: 'bob@example.test', to: 'alice@example.test', subject: 'Encrypted on the page' }, text: 'For Alice only.\r\n', encrypt: [alice] });
  fs.writeFileSync(path.join(tmp, 'page-encrypted.eml'), encryptedText, 'binary');
  r = openssl(['cms', '-decrypt', '-in', path.join(tmp, 'page-encrypted.eml'), '-recip', path.join(fixtures, 'alice.pem'), '-inkey', path.join(fixtures, 'alice-key.pem'), '-out', path.join(tmp, 'page-decrypted.txt')]);
  check(r.ok, 'openssl cms -decrypt opens the page\'s encrypted message' + (r.ok ? '' : ': ' + r.out.trim()));
  check(r.ok && /For Alice only/.test(fs.readFileSync(path.join(tmp, 'page-decrypted.txt'), 'binary')), 'and finds the text');
  const roundTrip = await smime.readMessage(smime.latin1Bytes(encryptedText), own);
  check(roundTrip.encrypted && roundTrip.encrypted.ok && roundTrip.content.text === 'For Alice only.\r\n', 'the module decrypts its own message');

  const bothText = await smime.composeMessage({ headers: { from: 'alice@example.test', to: 'alice@example.test', subject: 'Both' }, text: 'Signed and encrypted.\r\n', sign: signer, encrypt: [alice] });
  fs.writeFileSync(path.join(tmp, 'page-both.eml'), bothText, 'binary');
  r = openssl(['cms', '-decrypt', '-in', path.join(tmp, 'page-both.eml'), '-recip', path.join(fixtures, 'alice.pem'), '-inkey', path.join(fixtures, 'alice-key.pem'), '-out', path.join(tmp, 'page-both-inner.eml')]);
  let r2 = r.ok ? openssl(['cms', '-verify', '-in', path.join(tmp, 'page-both-inner.eml'), '-CAfile', path.join(fixtures, 'ca.pem'), '-out', path.join(tmp, 'page-both-content.txt')]) : r;
  check(r.ok && r2.ok && /Signed and encrypted/.test(fs.readFileSync(path.join(tmp, 'page-both-content.txt'), 'binary')), 'signed then encrypted on the page: OpenSSL decrypts, then verifies' + (r2.ok ? '' : ': ' + r2.out.trim()));
  const bothBack = await smime.readMessage(smime.latin1Bytes(bothText), own);
  check(bothBack.layers.join('>') === 'encrypted>signed' && bothBack.signed.valid && bothBack.encrypted.ok, 'and the module reads it back: ' + bothBack.layers.join(' > '));

  console.log('== a request for the domain\'s S/MIME authority');
  // What the webmail sends POST /api/v1/me/smime/enroll: a PKCS#10 request for a
  // key made on the page. OpenSSL verifies the request's self-signature, reads
  // its subject and its subjectAltName, and signs it as the fixture CA; the key
  // the page kept then matches the certificate, signs what OpenSSL verifies and,
  // for RSA, opens what OpenSSL encrypted to it.
  const caTmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hm-smime-ca-'));
  for (const keyType of ['rsa', 'ec']) {
    const made = await smime.makeRequest({ keyType: keyType, address: 'Carol@Example.Test', name: 'Carol Example' });
    check(made.keyType === keyType && made.pkcs8.length > 0 && made.request.length > 0, keyType + ': a key pair and a request were made');
    const der = path.join(caTmp, 'request-' + keyType + '.der');
    const pem = path.join(caTmp, 'request-' + keyType + '.pem');
    fs.writeFileSync(der, Buffer.from(made.request));
    r = openssl(['req', '-inform', 'DER', '-in', der, '-outform', 'PEM', '-out', pem]);
    check(r.ok, keyType + ': OpenSSL reads the request as DER' + (r.ok ? '' : ': ' + r.out.trim()));
    // OpenSSL 3 says the verdict on its standard error and may exit 0 either
    // way, so the words are what is read.
    let said = verdictOf(['req', '-in', pem, '-verify', '-noout']);
    check(/verify OK/i.test(said) && !/fail|does not match/i.test(said), keyType + ': openssl req -verify accepts the request\'s self-signature: ' + said.trim());
    r = openssl(['req', '-in', pem, '-noout', '-text']);
    check(r.ok && /CN\s*=\s*Carol Example/.test(r.out) && /emailAddress\s*=\s*carol@example\.test/.test(r.out) && /email:carol@example\.test/.test(r.out),
      keyType + ': the subject names the person and the lower-cased address, and the requested subjectAltName carries it');
    check(r.ok && (keyType === 'rsa' ? /Public-Key: \(2048 bit\)/.test(r.out) : /NIST CURVE: P-256|prime256v1/.test(r.out)),
      keyType + ': ' + (keyType === 'rsa' ? 'an RSA-2048 key' : 'a P-256 key'));
    // A request with a byte of its signed part changed does not verify: the
    // check above proves something.
    const broken = Buffer.from(made.request);
    const cn = broken.indexOf(Buffer.from('Carol'));
    broken[cn] = 'K'.charCodeAt(0);
    const brokenDer = path.join(caTmp, 'broken-' + keyType + '.der');
    fs.writeFileSync(brokenDer, broken);
    said = verdictOf(['req', '-inform', 'DER', '-in', brokenDer, '-verify', '-noout']);
    check(!/verify OK/i.test(said) && /fail|does not match|error/i.test(said), keyType + ': a request changed after it was signed is refused by OpenSSL (negative control): ' + said.trim().split(/\r?\n/).pop());
    const certificatePem = path.join(caTmp, 'issued-' + keyType + '.pem');
    r = openssl(['x509', '-req', '-in', pem, '-CA', path.join(fixtures, 'ca.pem'), '-CAkey', path.join(fixtures, 'ca-key.pem'), '-days', '2', '-copy_extensions', 'copy', '-set_serial', '4242', '-out', certificatePem]);
    check(r.ok, keyType + ': OpenSSL signs it as a CA would' + (r.ok ? '' : ': ' + r.out.trim()));
    if (!r.ok) { continue; }
    const issued = (await smime.importAny(new Uint8Array(fs.readFileSync(certificatePem)), '')).certificates[0];
    const kept = await smime.importPrivateKey(made.pkcs8);
    check(issued && issued.emails.indexOf('carol@example.test') >= 0 && smime.keyMatchesCertificate(kept, issued), keyType + ': the key the page kept matches the certificate issued for its request');
    const wrappedKey = await smime.wrapPrivateKey(made.pkcs8, 'account-password');
    check(Buffer.compare(Buffer.from(await smime.unwrapPrivateKey(wrappedKey, 'account-password')), Buffer.from(made.pkcs8)) === 0, keyType + ': it wraps and unwraps as an imported key does');
    const carolSigned = await smime.composeMessage({ headers: { from: 'carol@example.test', to: 'bob@example.test', subject: 'Enrolled' }, text: 'Signed with an enrolled key.\r\n', sign: { key: kept, certificate: issued, chain: [ca] } });
    fs.writeFileSync(path.join(caTmp, 'carol-' + keyType + '.eml'), carolSigned, 'binary');
    r = openssl(['cms', '-verify', '-in', path.join(caTmp, 'carol-' + keyType + '.eml'), '-CAfile', path.join(fixtures, 'ca.pem'), '-out', path.join(caTmp, 'carol-' + keyType + '.txt')]);
    check(r.ok, keyType + ': openssl cms -verify accepts a message signed with the enrolled key' + (r.ok ? '' : ': ' + r.out.trim()));
    if (keyType === 'rsa') {
      const toCarol = await smime.composeMessage({ headers: { from: 'bob@example.test', to: 'carol@example.test', subject: 'For Carol' }, text: 'For Carol only.\r\n', encrypt: [issued] });
      fs.writeFileSync(path.join(caTmp, 'to-carol.eml'), toCarol, 'binary');
      const opened = await smime.readMessage(smime.latin1Bytes(toCarol), [{ key: kept, certificate: issued }]);
      check(opened.encrypted && opened.encrypted.ok && opened.content.text === 'For Carol only.\r\n', 'rsa: a message encrypted to the enrolled certificate opens with the key the page kept');
    }
  }
  let refusedAddress = '';
  try { await smime.makeRequest({ keyType: 'rsa', address: 'not an address', name: 'x' }); } catch (e) { refusedAddress = e.message; }
  check(/address/.test(refusedAddress), 'a request for something that is not an ASCII address is refused on the page: ' + refusedAddress);
  fs.rmSync(caTmp, { recursive: true, force: true });

  console.log('== MIME details');
  const qp = smime.qpEncode(smime.utf8('café = ok \r\nline two'));
  check(qp === 'caf=C3=A9 =3D ok=20\r\nline two', 'quoted-printable: ' + JSON.stringify(qp));
  check(smime.decodeHeaderValue('=?utf-8?B?Y2Fmw6k=?= =?utf-8?Q?_ok?=') === 'café ok', 'RFC 2047 words decode');
  const ct = smime.parseMime(smime.utf8('Content-Type: text/plain; charset="utf-8"; name*=utf-8\'\'r%C3%A9sum%C3%A9.txt\r\n\r\nx'));
  check(ct.contentType.params.name === 'résumé.txt', 'RFC 2231 names decode: ' + ct.contentType.params.name);

  // A signature's logo, as the page's cleaner writes it, named twice.
  const logo = 'iVBORw0KGgo=';
  const withLogo = { text: 'Ada\r\n', html: '<p>Ada</p><img src="data:image/png;base64,' + logo + '" alt=""><img alt="" src="data:image/png;base64,' + logo + '">' };
  const builtText = text(smime.buildMime(withLogo));
  const built = smime.extractContent(smime.parseMime(smime.buildMime(withLogo)));
  check(/^Content-Type: multipart\/alternative/.test(builtText) && /Content-Type: multipart\/related; type="text\/html"/.test(builtText) && builtText.indexOf('data:image') < 0, 'an image the HTML holds as a data: URL goes as a part of multipart/related, and no data: URL is left');
  const logoId = built.attachments.length === 1 ? built.attachments[0].contentId : '';
  check(built.attachments.length === 1 && built.attachments[0].type === 'image/png' && Buffer.from(built.attachments[0].bytes).toString('base64') === logo, 'the image named twice is one PNG part with the bytes the URL held: ' + built.attachments.length + ' part(s)');
  check(!!logoId && built.html.split('cid:' + logoId).length === 3 && built.text === 'Ada\r\n', 'both cid: URLs in the HTML name that part\'s Content-ID, and the text is untouched: ' + logoId);
  // Sixty-five different images: sixty-four parts, as the server moves, and the last left as its data: URL.
  let manyHtml = '';
  for (let i = 0; i <= 64; i++) {
    manyHtml += '<img src="data:image/png;base64,' + Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 65 + (i % 26), 97 + Math.floor(i / 26)]).toString('base64') + '" alt="">';
  }
  const many = smime.extractContent(smime.parseMime(smime.buildMime({ text: 'x', html: manyHtml })));
  check(many.attachments.length === 64 && many.html.split('data:image/png').length === 2 && many.html.split('cid:').length === 65, 'at most 64 images become parts and the next stays a data: URL, as on the server: ' + many.attachments.length + ' part(s)');
  const untouchedHtml = '<img data-src="data:image/png;base64,' + logo + '"><img src="data:image/svg+xml;base64,PHN2Zz4=">';
  check(smime.extractContent(smime.parseMime(smime.buildMime({ text: 'x', html: untouchedHtml }))).html === untouchedHtml, 'a data-src and an SVG are left as they were');

  // A part with no name of its own: saved as "part" with the extension of its
  // declared type when the type is on the module's short list, and as "part"
  // alone otherwise - never with an extension a browser renders or a desktop
  // runs - and a named part keeps its name. Until 6.3.6 every nameless part was
  // "part", which opened in nothing once the page stopped handing the browser
  // the part's own type.
  const nameless = (type, extra) => 'MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary="b"\r\n\r\n--b\r\nContent-Type: text/plain\r\n\r\nbody\r\n' +
    '--b\r\nContent-Type: ' + type + '\r\nContent-Disposition: attachment' + (extra || '') + '\r\nContent-Transfer-Encoding: base64\r\n\r\nQUJD\r\n--b--\r\n';
  const savedAs = (type, extra) => {
    const parts = smime.extractContent(smime.parseMime(Buffer.from(nameless(type, extra), 'latin1'))).attachments;
    return parts.length === 1 ? parts[0].name : '(' + parts.length + ' parts)';
  };
  check(savedAs('application/pdf') === 'part.pdf' && savedAs('IMAGE/PNG') === 'part.png' && savedAs('text/calendar; method=REQUEST') === 'part.ics' &&
        savedAs('application/vnd.openxmlformats-officedocument.wordprocessingml.document') === 'part.docx',
        'a nameless part of a listed type is saved with its extension: ' + [savedAs('application/pdf'), savedAs('IMAGE/PNG'), savedAs('text/calendar; method=REQUEST')].join(', '));
  check(savedAs('text/html') === 'part' && savedAs('image/svg+xml') === 'part' && savedAs('application/x-msdownload') === 'part' &&
        savedAs('application/javascript') === 'part' && savedAs('application/hta') === 'part' && savedAs('application/x-unknown') === 'part',
        'a nameless part of any other type is saved as "part" alone: ' + [savedAs('text/html'), savedAs('image/svg+xml'), savedAs('application/x-msdownload')].join(', '));
  check(savedAs('application/pdf', '; filename="report.html"') === 'report.html' && savedAs('application/x-msdownload', '; filename="tool.exe"') === 'tool.exe',
        'a named part keeps its own name, as a named attachment always has');
  const signedLogo = await smime.composeMessage({ headers: { from: 'alice@example.test', to: 'bob@example.test', subject: 'Logo' }, text: withLogo.text, html: withLogo.html, sign: signer });
  const logoTmp = fs.mkdtempSync(path.join(os.tmpdir(), 'hm-smime-logo-'));
  fs.writeFileSync(path.join(logoTmp, 'logo.eml'), signedLogo, 'binary');
  r = openssl(['cms', '-verify', '-in', path.join(logoTmp, 'logo.eml'), '-CAfile', path.join(fixtures, 'ca.pem'), '-out', path.join(logoTmp, 'logo-content.txt')]);
  check(r.ok && /multipart\/related/.test(fs.readFileSync(path.join(logoTmp, 'logo-content.txt'), 'binary')), 'signed with its logo as a related part, OpenSSL still verifies it' + (r.ok ? '' : ': ' + r.out.trim()));
  fs.rmSync(logoTmp, { recursive: true, force: true });

  fs.rmSync(tmp, { recursive: true, force: true });
  if (madeFixtures) { fs.rmSync(fixtures, { recursive: true, force: true }); }
  console.log(checks + ' checks, ' + failures + ' failures');
  process.exit(failures ? 1 : 0);
})().catch(function (e) { console.log('ERROR ' + (e && e.stack || e)); process.exit(2); });
