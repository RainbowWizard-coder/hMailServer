// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Runs the page's OpenPGP module (PortalOpenPgp.js) in Node against artefacts
// GnuPG 2.4 made - keys on Curve25519 and RSA, detached signatures, cleartext
// signatures, messages encrypted with and without a signature, with ZIP, with
// ZLIB and with no compression, under AES-128 and AES-256, to one recipient
// and to two, a message encrypted to a passphrase, a revoked key, an expired
// key and two whole PGP/MIME messages - and, where it finds gpg on the PATH,
// hands what the module makes back to GnuPG: a key it generated, imported both
// as a public key and as a secret one; a detached signature; a message
// encrypted and signed to GnuPG's own key; a PGP/MIME message; and a
// revocation certificate. Both directions, against an implementation that is
// not this one.
//
// And two more things since the zero-access row (docs/ZeroAccess.md): the
// module's BLAKE2b and Argon2 against the vectors of RFC 7693 and RFC 9106,
// which is how the derivation a mailbox key is sealed under is held to the
// RFCs rather than to itself; and a PGP/MIME message the SERVER's writer
// made (build/fixtures/openpgp/server-sealed-to-alice.eml, from the Linux
// build's openpgp-writer) opened by the module with Alice's key - and by gpg,
// where there is one - which is the page reading what a delivery to a
// zero-access mailbox writes.
//
// Usage: node build/check-portal-openpgp.js [module.js] [fixtures dir]
// Needs Node 20 or later (Web Crypto, Ed25519 and X25519). gpg is optional:
// the committed fixtures are what the style workflow reads, so the check runs
// with no GnuPG installed - it then says which half it could not run.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const modulePath = process.argv[2] || path.join(__dirname, '..', 'hmailserver', 'source', 'Server', 'Common', 'Util', 'PortalOpenPgp.js');
const pgp = require(path.resolve(modulePath));
const fixtures = process.argv[3] || path.join(__dirname, 'fixtures', 'openpgp');

// What build/make-openpgp-fixtures.sh used; the README beside the fixtures
// says the same, and says why a passphrase in a source file is fine here.
const PASS = 'openpgp-fixture-pass';
const SYMPASS = 'openpgp-fixture-symmetric';
const BODY = 'A message GnuPG made for the page to read.\r\nSecond line, with caf\u00e9 and a tab:\tthere.\r\n';

let failures = 0;
let checks = 0;
function check(condition, what) {
  checks++;
  if (condition) { console.log('  ok   ' + what); } else { failures++; console.log('  FAIL ' + what); }
}
function read(name) { return new Uint8Array(fs.readFileSync(path.join(fixtures, name))); }
function readText(name) { return fs.readFileSync(path.join(fixtures, name), 'utf8'); }
function text(bytes) { return Buffer.from(bytes).toString('utf8'); }

function haveGpg() {
  try { execFileSync('gpg', ['--version'], { stdio: ['ignore', 'pipe', 'ignore'] }); return true; }
  catch (e) { return false; }
}

function gpg(home, args, input, passphrase) {
  const all = ['--batch', '--yes', '--no-tty', '--pinentry-mode', 'loopback', '--homedir', home];
  if (passphrase !== undefined) { all.push('--passphrase', passphrase); }
  try {
    const out = execFileSync('gpg', all.concat(args), { input: input, stdio: ['pipe', 'pipe', 'pipe'] });
    return { ok: true, out: out, text: out.toString('utf8'), err: '' };
  }
  catch (e) {
    return { ok: false, out: e.stdout || Buffer.alloc(0), text: (e.stdout || Buffer.alloc(0)).toString('utf8'), err: (e.stderr ? e.stderr.toString('utf8') : '') };
  }
}

// gpg writes its verdicts to stderr, so the interesting lines come back there
// even on a call that succeeded; execFileSync gives them only on a failure.
// --status-fd 1 puts the machine-readable ones on stdout instead, which is
// what every check below reads.
function gpgStatus(home, args, input, passphrase) {
  return gpg(home, ['--status-fd', '1'].concat(args), input, passphrase);
}

(async function () {
  console.log('== the module');
  check(pgp.available === true, 'the module has its cryptography');
  const curves = await pgp.curve25519Available();
  check(curves === true, 'this Node has Ed25519 and X25519');

  console.log('== keys GnuPG made');
  const alice = await pgp.readKey(readText('alice.pub.asc'));
  const aliceFpr = readText('alice.fpr').trim().toLowerCase();
  check(alice.fingerprint === aliceFpr, 'the Curve25519 key\'s fingerprint is the one GnuPG prints: ' + pgp.fingerprintText(alice.fingerprint));
  check(alice.algorithm === 'Ed25519', 'the primary is Ed25519');
  check(alice.userId === 'Alice Fixture <alice@fixtures.test>', 'the user id reads back: ' + alice.userId);
  check(alice.address === 'alice@fixtures.test', 'the address comes out of the user id');
  check(alice.subkeys.length === 1 && alice.subkeys[0].algorithm === pgp.ALGO.ECDH, 'it carries one ECDH subkey');
  check(alice.verified === true, 'every self-signature on it verified');
  check((alice.flags & pgp.FLAG.SIGN) !== 0, 'the primary may sign');
  check(pgp.encryptionKeyPacket(alice) === alice.subkeys[0], 'the subkey is the one to encrypt to');
  check(alice.preferences.sym.length > 0, 'it says which ciphers it takes');

  const bob = await pgp.readKey(readText('bob.pub.asc'));
  check(bob.fingerprint === readText('bob.fpr').trim().toLowerCase(), 'the RSA key\'s fingerprint is GnuPG\'s');
  check(bob.algorithm === 'RSA 3072', 'the RSA key is 3072 bits');
  check(bob.subkeys.length === 1, 'it carries one RSA subkey');
  check(bob.verified === true, 'every self-signature on the RSA key verified');

  const carol = await pgp.readKey(readText('carol.pub.asc'));
  check(carol.revoked === false, 'Carol\'s key before her revocation is not revoked');
  const carolRevoked = await pgp.readKey(readText('carol.revoked.pub.asc'));
  check(carolRevoked.revoked === true, 'Carol\'s key after it is revoked');
  check(pgp.keyUsable(carolRevoked) === false && pgp.keyProblem(carolRevoked) === 'the key is revoked', 'a revoked key is refused by name');
  await pgp.applyRevocation(carol, readText('carol.revocation.asc'));
  check(carol.revoked === true, 'GnuPG\'s revocation certificate, applied here, revokes the key');
  const reapplied = await pgp.readKey(pgp.dearmor(pgp.exportPublic(carol)).bytes);
  check(reapplied.revoked === true, 'and the export carries the revocation on to whoever fetches it');

  const dave = await pgp.readKey(readText('dave.pub.asc'));
  check(pgp.keyExpired(dave) === true && pgp.keyProblem(dave) === 'the key expired', 'an expired key is refused by name');

  console.log('== secret keys GnuPG made');
  const aliceSecret = await pgp.readKey(readText('alice.sec.asc'), PASS);
  check(aliceSecret.unlocked === true && aliceSecret.fingerprint === aliceFpr, 'the Curve25519 secret key opens with its passphrase');
  const bobSecret = await pgp.readKey(readText('bob.sec.asc'), PASS);
  check(bobSecret.unlocked === true, 'the RSA secret key opens with its passphrase');
  let refused = '';
  try { await pgp.readKey(readText('alice.sec.asc'), 'not the passphrase'); }
  catch (e) { refused = e.message; }
  check(refused === 'wrong passphrase', 'a wrong passphrase is refused as "wrong passphrase", not as a broken key');
  let notAKey = '';
  try { await pgp.readKey(readText('encrypted-to-alice-plain.asc')); }
  catch (e) { notAKey = e.message; }
  check(notAKey === 'this is a message, not a key', 'a message offered as a key is refused in those words');

  console.log('== signatures GnuPG made');
  const body = read('body.bin');
  check(text(body) === BODY, 'the fixture body is the bytes this check expects');
  const ed = await pgp.verifyDetached(readText('detached-ed25519.sig'), body, [alice]);
  check(ed.valid === true, 'the Ed25519 detached signature verifies: ' + (ed.error || 'good'));
  check(ed.keyId === aliceFpr.slice(-16), 'and it names Alice\'s key');
  const rsa = await pgp.verifyDetached(readText('detached-rsa.sig'), body, [bob]);
  check(rsa.valid === true, 'the RSA detached signature verifies: ' + (rsa.error || 'good'));
  const tampered = new Uint8Array(body);
  tampered[3] ^= 0x20;
  const bad = await pgp.verifyDetached(readText('detached-ed25519.sig'), tampered, [alice]);
  check(bad.valid === false, 'one byte changed in the body and the signature no longer verifies');
  const stranger = await pgp.verifyDetached(readText('detached-ed25519.sig'), body, [bob]);
  check(stranger.valid === false && stranger.error.indexOf('no key here is') === 0, 'a signature by a key nobody has says so: ' + stranger.error);

  console.log('== messages GnuPG made');
  const toAlice = await pgp.decryptBytes(readText('encrypted-to-alice-signed.asc'), [aliceSecret, bob]);
  check(text(toAlice.data) === BODY, 'the message encrypted to the Curve25519 key opens');
  check(toAlice.signed && toAlice.signed.valid === true, 'and the RSA signature inside it verifies: ' + (toAlice.signed ? toAlice.signed.error || 'good' : 'none'));
  check(toAlice.compressed === true, 'GnuPG compressed it, and the page decompressed it');
  const toBob = await pgp.decryptBytes(readText('encrypted-to-bob-signed.asc'), [bobSecret, alice]);
  check(text(toBob.data) === BODY, 'the message encrypted to the RSA key opens');
  check(toBob.signed && toBob.signed.valid === true, 'and the Curve25519 signature inside it verifies');
  for (const kind of ['plain', 'zlib', 'zip', 'aes128']) {
    const opened = await pgp.decryptBytes(readText('encrypted-to-alice-' + kind + '.asc'), [aliceSecret]);
    check(text(opened.data) === BODY, 'the ' + kind + ' message opens');
  }
  const bothA = await pgp.decryptBytes(readText('encrypted-to-both.asc'), [aliceSecret]);
  const bothB = await pgp.decryptBytes(readText('encrypted-to-both.asc'), [bobSecret]);
  check(text(bothA.data) === BODY && text(bothB.data) === BODY, 'a message to two recipients opens with either key');
  const wrongKey = await pgp.decryptBytes(readText('encrypted-to-alice-plain.asc'), [aliceSecret]).then(function () { return ''; }, function (e) { return e.message; });
  check(wrongKey === '', 'the right key opens it');
  const noKey = await pgp.decryptBytes(readText('encrypted-to-alice-plain.asc'), [bobSecret]).then(function () { return ''; }, function (e) { return e.message; });
  check(noKey.indexOf('no key here is one of them') > 0, 'the wrong key says which key ids the message was for: ' + noKey);
  const symmetric = await pgp.decryptBytes(readText('symmetric.asc'), [], [SYMPASS]);
  check(text(symmetric.data) === BODY, 'the message encrypted to a passphrase opens with that passphrase');
  const symmetricWrong = await pgp.decryptBytes(readText('symmetric.asc'), [], ['not it']).then(function () { return ''; }, function (e) { return e.message; });
  check(symmetricWrong !== '', 'and not with another: ' + symmetricWrong);

  // The modification detection code: one byte of the ciphertext changed, and
  // the message must not be handed to the reader at all.
  const sealed = pgp.dearmor(readText('encrypted-to-alice-plain.asc')).bytes;
  const cut = new Uint8Array(sealed);
  cut[cut.length - 30] ^= 0x01;
  const altered = await pgp.decryptBytes(cut, [aliceSecret]).then(function () { return ''; }, function (e) { return e.message; });
  check(altered.indexOf('altered') > 0 || altered.indexOf('did not open') > 0, 'a message changed after it was encrypted is refused: ' + altered);

  console.log('== PGP/MIME GnuPG made');
  const signedMessage = await pgp.readMessage(read('pgpmime-signed.eml'), [alice]);
  check(signedMessage.layers.join(',') === 'signed', 'the multipart/signed message is seen as signed');
  check(signedMessage.signed.valid === true, 'its signature verifies: ' + (signedMessage.signed.error || 'good'));
  check(signedMessage.content.text === BODY, 'and the text under it is the body');
  const encryptedMessage = await pgp.readMessage(read('pgpmime-encrypted.eml'), [aliceSecret, bob]);
  check(encryptedMessage.layers.join(',') === 'encrypted,signed', 'the multipart/encrypted message is seen as encrypted and signed');
  check(encryptedMessage.encrypted.ok === true, 'it decrypts');
  check(encryptedMessage.signed.valid === true, 'the signature inside it verifies');
  check(encryptedMessage.content.text === BODY, 'and the text under it is the body');
  const noKeyMessage = await pgp.readMessage(read('pgpmime-encrypted.eml'), [bobSecret]);
  check(noKeyMessage.encrypted.ok === false && noKeyMessage.encrypted.error.length > 0, 'a message to somebody else says why it did not open');

  console.log('== inline PGP GnuPG made');
  const cleartext = await pgp.readMessage(pgp.utf8('Content-Type: text/plain\r\n\r\n' + readText('cleartext-ed25519.asc')), [alice]);
  check(cleartext.inline === true && cleartext.signed.valid === true, 'a cleartext-signed body verifies: ' + (cleartext.signed.error || 'good'));
  check(cleartext.content.text.indexOf('A message GnuPG made') === 0, 'and the dash-escaping comes off the text');
  const inlineEncrypted = await pgp.readMessage(pgp.utf8('Content-Type: text/plain\r\n\r\n' + readText('encrypted-to-alice-plain.asc')), [aliceSecret]);
  check(inlineEncrypted.inline === true && inlineEncrypted.encrypted.ok === true, 'an inline PGP MESSAGE in the body opens');
  check(inlineEncrypted.content.text === BODY, 'and its text is the body');

  console.log('== Autocrypt');
  const header = pgp.autocryptHeader('alice@fixtures.test', alice, true);
  check(header.indexOf('addr=alice@fixtures.test; prefer-encrypt=mutual; keydata=') === 0, 'the header begins with addr and prefer-encrypt');
  check(header.split('\r\n').every(function (line) { return line.length <= 78; }), 'and it is folded, so no line is past what RFC 5322 allows');
  const parsed = pgp.parseAutocrypt(header);
  check(parsed && parsed.address === 'alice@fixtures.test' && parsed.preferEncrypt === true, 'it parses back');
  const learned = await pgp.readKey(parsed.keydata);
  check(learned.fingerprint === alice.fingerprint, 'and the key in it is the same key');
  check(parsed.keydata.length < pgp.publicBytes(alice).length + 1, 'the key in the header is the minimal one');
  check(pgp.parseAutocrypt('addr=a@b.test; keydata=' + Buffer.from('x').toString('base64') + '; unknown=1') === null, 'a header with an unknown attribute is ignored, as Autocrypt Level 1 says');
  check(pgp.parseAutocrypt('prefer-encrypt=mutual') === null, 'a header with no addr and no keydata is ignored');

  console.log('== BLAKE2b and Argon2, against the RFCs');
  // RFC 7693 appendix A: BLAKE2b-512 of "abc".
  const blakeAbc = Buffer.from(pgp.blake2b(pgp.utf8('abc'), 64)).toString('hex');
  check(blakeAbc === 'ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923',
    'BLAKE2b-512 of "abc" is RFC 7693 appendix A\'s');
  // Two blocks and exactly one, against Node's own BLAKE2b.
  const nodeCrypto = require('crypto');
  const twoBlocks = new Uint8Array(200); for (let i = 0; i < 200; i++) { twoBlocks[i] = i; }
  check(Buffer.from(pgp.blake2b(twoBlocks, 64)).toString('hex') === nodeCrypto.createHash('blake2b512').update(twoBlocks).digest('hex'), 'BLAKE2b over two blocks agrees with Node\'s');
  const oneBlock = new Uint8Array(128).fill(7);
  check(Buffer.from(pgp.blake2b(oneBlock, 64)).toString('hex') === nodeCrypto.createHash('blake2b512').update(oneBlock).digest('hex'), 'and over exactly one block');
  check(Buffer.from(pgp.blake2b(new Uint8Array(0), 32)).toString('hex') === '0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8', 'BLAKE2b-256 of nothing is the known value');
  // RFC 9106 sections 5.1 to 5.3: the three variants, with the secret and the
  // associated data the RFC's vectors carry, t=3, m=32 KiB, p=4, 32 octets.
  const rfcPassword = new Uint8Array(32).fill(1), rfcSalt = new Uint8Array(16).fill(2), rfcSecret = new Uint8Array(8).fill(3), rfcAd = new Uint8Array(12).fill(4);
  const argonVectors = [
    ['Argon2d', pgp.ARGON2.D, '512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb'],
    ['Argon2i', pgp.ARGON2.I, 'c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8'],
    ['Argon2id', pgp.ARGON2.ID, '0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659']
  ];
  for (const [name, type, want] of argonVectors) {
    const tag = Buffer.from(await pgp.argon2({ password: rfcPassword, salt: rfcSalt, secret: rfcSecret, ad: rfcAd, passes: 3, memory: 32, lanes: 4, length: 32, type: type, yield: false })).toString('hex');
    check(tag === want, name + ' reproduces RFC 9106\'s test vector');
  }
  // And at the parameters a mailbox key is actually sealed under - three
  // passes over 64 MiB in four lanes - against a reference implementation
  // (argon2-cffi 25.1, the reference C code): the RFC's vectors run at 32 KiB,
  // where the reference index of section 3.4.1.2 is exercised over a few
  // hundred blocks and never at the sizes where its 64-bit square matters.
  // This takes a second or two, which is what a sign-in costs.
  const bigTag = await pgp.zeroAccessPassphrase('correct horse battery staple', 'BRIfLDlGU2BteoeUoa67yA==', { passes: 3, memory: 65536, lanes: 4 });
  check(bigTag === 'c4e2377097b2e89ada4b2fbcc4788be511a5fedda7d1f94d88be76b0dcf69c65', 'Argon2id at 64 MiB, three passes, four lanes agrees with the reference implementation');
  // What a mailbox key is sealed under: 64 hex characters, the same for the
  // same password and salt, different for a different salt.
  const salt1 = pgp.b64encode(new Uint8Array(16).fill(9));
  const salt2 = pgp.b64encode(new Uint8Array(16).fill(10));
  const smallParams = { passes: 1, memory: 256, lanes: 1 };
  const pass1 = await pgp.zeroAccessPassphrase('correct horse', salt1, smallParams);
  const pass1again = await pgp.zeroAccessPassphrase('correct horse', salt1, smallParams);
  const pass2 = await pgp.zeroAccessPassphrase('correct horse', salt2, smallParams);
  check(/^[0-9a-f]{64}$/.test(pass1) && pass1 === pass1again && pass1 !== pass2, 'the derived passphrase is 64 hex characters, repeatable, and salted');
  const recovery = pgp.makeRecoveryKey();
  check(/^([A-Z2-7]{6}-){7}[A-Z2-7]{6}$/.test(recovery), 'a recovery key is eight groups of six base32 characters: ' + recovery);
  check(pgp.canonicalRecoveryKey(recovery.toLowerCase().replace(/-/g, ' ')) === recovery.replace(/-/g, ''), 'and reads back in any case, with or without its separators');
  check(pgp.canonicalRecoveryKey(recovery.slice(0, 40)) === '', 'a recovery key with a group missing is refused');
  check(pgp.makeRecoveryKey() !== recovery, 'two recovery keys differ');
  // A key sealed under the derived passphrase and under the recovery key is
  // an ordinary OpenPGP secret key that opens with either, as GnuPG would.
  const sealedUnderPassword = await pgp.exportSecret(aliceSecret, pass1);
  const reopened = await pgp.readKey(sealedUnderPassword, pass1);
  check(reopened.unlocked === true && reopened.fingerprint === aliceFpr, 'Alice\'s key sealed under the derived passphrase opens with it');
  const wrongPassphrase = await pgp.readKey(sealedUnderPassword, pass2).then(function () { return ''; }, function (e) { return e.message; });
  check(wrongPassphrase === 'wrong passphrase', 'and not with the passphrase of another salt');
  const sealedUnderRecovery = await pgp.exportSecret(aliceSecret, pgp.canonicalRecoveryKey(recovery));
  const reopenedRecovery = await pgp.readKey(sealedUnderRecovery, pgp.canonicalRecoveryKey(recovery));
  check(reopenedRecovery.unlocked === true, 'and sealed under the recovery key, opens with it');

  console.log('== what the server\'s writer sealed, read by the page');
  // build/fixtures/openpgp/server-sealed-to-alice.eml was written by the
  // server's OpenPgpWriter (the Linux build's openpgp-writer, --mime) over a
  // small message to Alice: what a zero-access mailbox's delivery looks like.
  const serverSealed = await pgp.readMessage(read('server-sealed-to-alice.eml'), [aliceSecret, bob]);
  check(serverSealed.layers.join(',') === 'encrypted', 'the server\'s PGP/MIME message is seen as encrypted, and not signed');
  check(serverSealed.encrypted && serverSealed.encrypted.ok === true, 'it decrypts with Alice\'s key: ' + (serverSealed.encrypted ? serverSealed.encrypted.error || 'good' : 'none'));
  check(serverSealed.content && serverSealed.content.text === 'The body goes under the key.\r\n', 'and the text under it is the original body: ' + JSON.stringify(serverSealed.content && serverSealed.content.text));
  check(serverSealed.compressed !== true, 'the server compressed nothing');
  const serverSealedText = readText('server-sealed-to-alice.eml');
  check(/^X-hMailServer-Zero-Access: sealed on delivery\r?$/m.test(serverSealedText) && /^Subject: A sealed delivery\r?$/m.test(serverSealedText), 'the outer headers carry the subject in the clear and the server\'s mark');
  check(serverSealedText.indexOf('The body goes under the key.') < 0, 'and the plaintext body is nowhere in the file');
  const serverSealedByBob = await pgp.readMessage(read('server-sealed-to-alice.eml'), [bobSecret]);
  check(serverSealedByBob.encrypted && serverSealedByBob.encrypted.ok === false, 'a key that is not the mailbox\'s does not open it');

  console.log('== keys and messages the page makes');
  const madeEd = await pgp.generateKey({ type: 'curve25519', name: 'Page Curve', email: 'page-curve@fixtures.test', passphrase: 'page-pass', expiresIn: 0 });
  const madeRsa = await pgp.generateKey({ type: 'rsa', bits: 2048, name: 'Page RSA', email: 'page-rsa@fixtures.test', passphrase: 'page-pass', expiresIn: 3600 * 24 * 365 });
  check(madeEd.fingerprint.length === 40 && madeRsa.fingerprint.length === 40, 'both made keys have a version 4 fingerprint');
  const readEd = await pgp.readKey(madeEd.publicArmored);
  check(readEd.fingerprint === madeEd.fingerprint && readEd.verified === true, 'the Curve25519 key reads back and its self-signatures verify');
  const readRsa = await pgp.readKey(madeRsa.publicArmored);
  check(readRsa.verified === true && readRsa.expires > Math.floor(Date.now() / 1000), 'the RSA key reads back with its expiry a year out');
  const openedEd = await pgp.readKey(madeEd.secretArmored, 'page-pass');
  check(openedEd.unlocked === true, 'its secret key opens with the passphrase the page wrapped it under');
  const mine = pgp.utf8('What the page wrote.\r\n');
  const sealedMine = await pgp.encryptBytes({ data: mine, recipients: [readEd, alice], signer: openedEd });
  const openedMine = await pgp.decryptBytes(sealedMine, [openedEd, readEd]);
  check(text(openedMine.data) === 'What the page wrote.\r\n' && openedMine.signed.valid === true, 'a message the page sealed to two keys opens and its signature verifies');
  const openedByAlice = await pgp.decryptBytes(sealedMine, [aliceSecret, readEd]);
  check(text(openedByAlice.data) === 'What the page wrote.\r\n', 'and the GnuPG recipient of the same message opens it here too');
  const composed = await pgp.composeMessage({ headers: { from: 'a@b.test', to: 'alice@fixtures.test', subject: 'hello' }, text: 'A PGP/MIME message.\n', sign: openedEd, encrypt: [alice] });
  const reread = await pgp.readMessage(pgp.utf8(composed), [aliceSecret, readEd]);
  check(reread.encrypted && reread.encrypted.ok === true && reread.signed && reread.signed.valid === true, 'a PGP/MIME message the page composed opens and verifies here');
  check(reread.content.text.indexOf('A PGP/MIME message.') === 0, 'and its text survives the round trip');
  const composedSigned = await pgp.composeMessage({ headers: {}, text: 'Signed only.\n', sign: openedEd, encrypt: null });
  const rereadSigned = await pgp.readMessage(pgp.utf8(composedSigned), [readEd]);
  check(rereadSigned.signed && rereadSigned.signed.valid === true && rereadSigned.content.text.indexOf('Signed only.') === 0, 'and a signed-only one does too');
  let revokedMine = await pgp.readKey(madeEd.publicArmored);
  await pgp.applyRevocation(revokedMine, madeEd.revocation);
  check(revokedMine.revoked === true, 'the revocation certificate made with the key revokes it');
  // A signature made before the revocation is still a good signature, and
  // saying it is not would be a lie about the arithmetic: valid stays true and
  // the reason to distrust it is reported beside it.
  const signedBefore = await pgp.signDetached(openedEd, mine);
  const afterRevocation = await pgp.verifyDetached(signedBefore.armored, mine, [revokedMine]);
  check(afterRevocation.valid === true && afterRevocation.problem.indexOf('revoked') > 0,
    'a signature by a key revoked since verifies, and says the key was revoked: ' + JSON.stringify([afterRevocation.valid, afterRevocation.problem]));
  const byLivingKey = await pgp.verifyDetached(signedBefore.armored, mine, [readEd]);
  check(byLivingKey.valid === true && byLivingKey.problem === '', 'and the same signature against the key before its revocation has nothing to report');
  const refusedRevoked = await pgp.encryptBytes({ data: mine, recipients: [revokedMine] }).then(function () { return ''; }, function (e) { return e.message; });
  check(refusedRevoked.indexOf('the key is revoked') === 0, 'and a revoked key encrypts nothing: ' + refusedRevoked);

  console.log('== a message to a password (the password-protected message)');
  // The shape the compose form makes for a correspondent with no key: the
  // text and the attachments as one MIME message, encrypted to a passphrase
  // and nothing else, and opened by the reading page with the same passphrase.
  const attachmentBytes = pgp.utf8('column,value\r\nfigures,42\r\n');
  const protectedArmor = await pgp.protectedMessage({
    headers: { subject: 'The board pack', from: 'sender@fixtures.test', to: 'reader@fixtures.test' },
    text: 'The figures are 42.\r\n', html: '',
    attachments: [{ name: 'figures.csv', type: 'text/csv', bytes: attachmentBytes }],
    password: 'correct-horse-battery'
  });
  check(protectedArmor.indexOf('-----BEGIN PGP MESSAGE-----') === 0 && protectedArmor.indexOf('The figures are 42.') < 0, 'the page makes an armoured message that carries none of the text');
  const protectedPackets = pgp.readPackets(pgp.dearmor(protectedArmor).bytes);
  check(protectedPackets.length === 2 && protectedPackets[0].tag === pgp.TAG.SKESK && protectedPackets[1].tag === pgp.TAG.SEIPD,
    'it is a symmetric-key encrypted session key followed by an integrity-protected body, and nothing else: ' + protectedPackets.map(function (p) { return p.tag; }).join(','));
  const openedProtected = await pgp.openProtected(protectedArmor, 'correct-horse-battery');
  check(openedProtected.subject === 'The board pack' && openedProtected.content.text === 'The figures are 42.\r\n', 'the reading page opens it with the password and gets the subject and the text back');
  check(openedProtected.content.attachments.length === 1 && openedProtected.content.attachments[0].name === 'figures.csv' && text(openedProtected.content.attachments[0].bytes) === text(attachmentBytes),
    'and the attachment, byte for byte');
  const wrongPassword = await pgp.openProtected(protectedArmor, 'not the password').then(function () { return ''; }, function (e) { return e.message; });
  check(wrongPassword === 'wrong passphrase', 'a wrong password is refused as "wrong passphrase", so the page can say so and nothing else: ' + wrongPassword);
  const noPassword = await pgp.protectedMessage({ headers: {}, text: 'x', password: '' }).then(function () { return ''; }, function (e) { return e.message; });
  check(noPassword === 'a password is needed', 'and nothing is made without a password');
  // The reading page fails closed: a message that was never encrypted - a
  // bare literal packet under the armour - and one encrypted without
  // integrity protection (the old tag-9 packet) are both refused in the one
  // sentence, since decryptBytes reads both and a page that showed either as
  // "opened with the password" would be lying.
  const literalBody = pgp.concat([new Uint8Array([0x62, 0]), new Uint8Array([0, 0, 0, 0]), pgp.utf8('Subject: Not sealed\r\n\r\nIn the clear.\r\n')]);
  const bareLiteral = pgp.armor('MESSAGE', pgp.writePacket(pgp.TAG.LITERAL, literalBody));
  const bareRefused = await pgp.openProtected(bareLiteral, 'any password').then(function () { return ''; }, function (e) { return e.message; });
  check(bareRefused === 'wrong passphrase', 'a message that was never encrypted is refused by the reading page in the one sentence: ' + bareRefused);
  const sessionKey = new Uint8Array(nodeCrypto.randomBytes(32));
  const skesk = await pgp.writeSkesk('the-password', pgp.SYM.AES256, sessionKey);
  // RFC 4880 section 5.7: the prefix under a zero IV, then the data under
  // the resynchronised IV that is the prefix's last sixteen ciphertext bytes.
  const prefix = new Uint8Array(18);
  prefix.set(nodeCrypto.randomBytes(16), 0);
  prefix[16] = prefix[14];
  prefix[17] = prefix[15];
  const first = nodeCrypto.createCipheriv('aes-256-cfb', Buffer.from(sessionKey), Buffer.alloc(16));
  const head = new Uint8Array(Buffer.concat([first.update(Buffer.from(prefix)), first.final()]));
  const second = nodeCrypto.createCipheriv('aes-256-cfb', Buffer.from(sessionKey), Buffer.from(head.subarray(2, 18)));
  const rest = new Uint8Array(Buffer.concat([second.update(Buffer.from(pgp.writePacket(pgp.TAG.LITERAL, literalBody))), second.final()]));
  const unprotected = pgp.armor('MESSAGE', pgp.concat([skesk, pgp.writePacket(pgp.TAG.SED, pgp.concat([head, rest]))]));
  const sedOpens = await pgp.decryptBytes(unprotected, [], ['the-password']);
  check(sedOpens.integrityProtected === false && text(sedOpens.data).indexOf('Subject: Not sealed') === 0, 'the module reads a message without integrity protection and says so');
  const sedRefused = await pgp.openProtected(unprotected, 'the-password').then(function () { return ''; }, function (e) { return e.message; });
  check(sedRefused === 'wrong passphrase', 'and the reading page refuses it in the one sentence rather than showing it: ' + sedRefused);

  console.log('== what GnuPG makes of what the page made');
  // A gpg that cannot start its agent cannot touch a secret key, and on this
  // project's Windows benches the one on the PATH is exactly that. So gpg is
  // proved to work here before anything is asked of it, and where it does not
  // this half is skipped by name rather than reported as a failure of the
  // module - the committed fixtures are the GnuPG half in that case, and the
  // style workflow's Ubuntu runner has a gpg that works.
  const home = haveGpg() ? fs.mkdtempSync(path.join(os.tmpdir(), 'hm-openpgp-')) : '';
  let gpgWhy = haveGpg() ? '' : 'gpg is not on the PATH';
  if (home) {
    fs.chmodSync(home, 0o700);
    gpg(home, ['--import'], Buffer.from(readText('alice.sec.asc') + readText('bob.sec.asc'), 'utf8'), PASS);
    const probe = gpg(home, ['--decrypt'], Buffer.from(readText('encrypted-to-alice-plain.asc'), 'utf8'), PASS);
    if (!probe.ok || probe.text.indexOf('A message GnuPG made') < 0) {
      gpgWhy = 'gpg is on the PATH but cannot use a secret key here (' + (probe.err.split('\n').filter(function (l) { return l.indexOf('gpg:') === 0; }).pop() || 'no reason given').trim() + ')';
    }
  }
  if (gpgWhy) {
    fs.rmSync(home || os.tmpdir() + '/does-not-exist', { recursive: true, force: true });
    console.log('  --   ' + gpgWhy + ': the committed fixtures are the GnuPG half of this check.');
    console.log('       Run build/make-openpgp-fixtures.sh where GnuPG 2.4 is to remake them.');
  }
  else {
    try {
      const imported = gpg(home, ['--import'], Buffer.from(madeEd.publicArmored + madeRsa.publicArmored, 'utf8'));
      check(imported.ok, 'GnuPG imports both public keys the page made' + (imported.ok ? '' : ': ' + imported.err.trim()));
      const listed = gpg(home, ['--with-colons', '--list-keys']);
      check(listed.text.toUpperCase().indexOf(madeEd.fingerprint.toUpperCase()) > 0, 'and the Curve25519 key is there under the fingerprint the page computed');
      check(listed.text.toUpperCase().indexOf(madeRsa.fingerprint.toUpperCase()) > 0, 'and so is the RSA one');

      const bodyFile = path.join(home, 'body.bin');
      fs.writeFileSync(bodyFile, Buffer.from(mine));
      const signature = await pgp.signDetached(openedEd, mine);
      const sigFile = path.join(home, 'ours.sig');
      fs.writeFileSync(sigFile, signature.armored);
      const verified = gpgStatus(home, ['--verify', sigFile, bodyFile]);
      check(verified.text.indexOf('GOODSIG') >= 0, 'GnuPG calls the page\'s detached signature good');

      const toGnupg = await pgp.encryptBytes({ data: mine, recipients: [alice], signer: openedEd });
      const decrypted = gpgStatus(home, ['--decrypt'], Buffer.from(pgp.armor('MESSAGE', toGnupg), 'utf8'), PASS);
      check(decrypted.text.indexOf('GOODSIG') >= 0, 'GnuPG decrypts the page\'s message to its own key and calls the signature good');
      check(decrypted.text.indexOf('What the page wrote.') >= 0, 'and the plaintext it gets is what the page wrote');

      const toGnupgRsa = await pgp.encryptBytes({ data: mine, recipients: [bob], signer: await pgp.readKey(madeRsa.secretArmored, 'page-pass') });
      const decryptedRsa = gpgStatus(home, ['--decrypt'], Buffer.from(pgp.armor('MESSAGE', toGnupgRsa), 'utf8'), PASS);
      check(decryptedRsa.text.indexOf('GOODSIG') >= 0 && decryptedRsa.text.indexOf('What the page wrote.') >= 0, 'the same over RSA: GnuPG decrypts it and calls the signature good');

      const secretImported = gpg(home, ['--import'], Buffer.from(madeEd.secretArmored, 'utf8'), 'page-pass');
      check(secretImported.ok, 'GnuPG imports the secret key the page made, wrapped under its passphrase');
      const signedThere = gpg(home, ['--local-user', madeEd.fingerprint.toUpperCase(), '--armor', '--detach-sign'], Buffer.from(mine), 'page-pass');
      check(signedThere.ok && signedThere.text.indexOf('-----BEGIN PGP SIGNATURE-----') === 0, 'and signs with it there');
      const backHere = await pgp.verifyDetached(signedThere.text, mine, [readEd]);
      check(backHere.valid === true, 'and that signature, made by GnuPG with the page\'s key, verifies here');

      const revokedThere = gpg(home, ['--import'], Buffer.from(madeRsa.revocation, 'utf8'));
      check(revokedThere.ok, 'GnuPG imports the page\'s revocation certificate');
      const afterRevocation = gpg(home, ['--with-colons', '--list-keys', madeRsa.fingerprint.toUpperCase()]);
      check(/^pub:r:/m.test(afterRevocation.text), 'and reports the key as revoked afterwards');

      // The server's writer, read by GnuPG: the inner armour of the fixture
      // the module opened above, handed to gpg with Alice's key.
      const serverArmour = readText('server-sealed-to-alice.eml').replace(/\r\n/g, '\n');
      const armourStart = serverArmour.indexOf('-----BEGIN PGP MESSAGE-----');
      const armourEnd = serverArmour.indexOf('-----END PGP MESSAGE-----');
      const serverOpened = gpg(home, ['--decrypt'], Buffer.from(serverArmour.slice(armourStart, armourEnd) + '-----END PGP MESSAGE-----\n', 'utf8'), PASS);
      check(serverOpened.ok && serverOpened.text.indexOf('The body goes under the key.') >= 0 && serverOpened.text.indexOf('Subject: A sealed delivery') >= 0,
        'GnuPG opens what the server\'s writer sealed, and finds the whole original message inside' + (serverOpened.ok ? '' : ': ' + serverOpened.err.trim()));

      const symmetricMine = await pgp.encryptBytes({ data: mine, recipients: [], passwords: ['page-symmetric'] });
      const symmetricThere = gpg(home, ['--decrypt'], Buffer.from(pgp.armor('MESSAGE', symmetricMine), 'utf8'), 'page-symmetric');
      check(symmetricThere.ok && symmetricThere.text.indexOf('What the page wrote.') >= 0, 'GnuPG opens the page\'s passphrase-encrypted message');
    }
    finally { fs.rmSync(home, { recursive: true, force: true }); }
  }

  console.log('== the password-protected message, against gpg -c');
  // A passphrase needs no secret key and no agent that can hold one, so this
  // half runs wherever gpg decrypts the committed symmetric fixture - which
  // includes a bench whose gpg cannot touch a secret key at all.
  const symHome = haveGpg() ? fs.mkdtempSync(path.join(os.tmpdir(), 'hm-openpgp-sym-')) : '';
  let symWhy = haveGpg() ? '' : 'gpg is not on the PATH';
  if (symHome) {
    fs.chmodSync(symHome, 0o700);
    const probe = gpg(symHome, ['--decrypt'], Buffer.from(readText('symmetric.asc'), 'utf8'), SYMPASS);
    if (!probe.ok || probe.text.indexOf('A message GnuPG made') < 0) {
      symWhy = 'gpg is on the PATH but cannot decrypt to a passphrase here (' + (probe.err.split('\n').filter(function (l) { return l.indexOf('gpg:') === 0; }).pop() || 'no reason given').trim() + ')';
    }
  }
  if (symWhy) {
    fs.rmSync(symHome || os.tmpdir() + '/does-not-exist', { recursive: true, force: true });
    console.log('  --   ' + symWhy + ': the module\'s own round trip above is what proves this half here.');
  }
  else {
    try {
      // Both ways: GnuPG opens what the compose form made and gets the MIME
      // message with the text in it, and the reading page opens what "gpg -c"
      // made of such a message.
      const protectedThere = gpg(symHome, ['--decrypt'], Buffer.from(protectedArmor, 'utf8'), 'correct-horse-battery');
      check(protectedThere.ok && protectedThere.text.indexOf('Subject: The board pack') >= 0 && protectedThere.text.indexOf('The figures are 42.') >= 0,
        'GnuPG opens the password-protected message with the password and gets the MIME message the page built');
      const protectedWrong = gpg(symHome, ['--decrypt'], Buffer.from(protectedArmor, 'utf8'), 'not the password');
      check(!protectedWrong.ok, 'and not with another password');
      const mimeForGpg = Buffer.from('From: sender@fixtures.test\r\nTo: reader@fixtures.test\r\nSubject: From gpg -c\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nSealed by GnuPG for the page.\r\n', 'utf8');
      const gpgSymmetric = gpg(symHome, ['--symmetric', '--armor', '--cipher-algo', 'AES256', '--s2k-digest-algo', 'SHA256'], mimeForGpg, 'gpg-chose-this');
      // Judged by what it wrote, not by its exit code: a gpg whose agent
      // cannot start writes the armour and then exits 2 complaining about
      // the cache it could not reach, and the page's opening of that armour
      // below is the proof that matters.
      check(gpgSymmetric.text.indexOf('-----BEGIN PGP MESSAGE-----') === 0, 'GnuPG makes a message to a passphrase with gpg -c');
      const openedFromGpg = await pgp.openProtected(gpgSymmetric.text, 'gpg-chose-this').then(function (r) { return r; }, function (e) { return { error: e.message }; });
      check(!openedFromGpg.error && openedFromGpg.subject === 'From gpg -c' && String(openedFromGpg.content.text).indexOf('Sealed by GnuPG for the page.') === 0,
        'and the reading page opens it with that passphrase and reads the message inside: ' + (openedFromGpg.error || 'opened'));
    }
    finally { fs.rmSync(symHome, { recursive: true, force: true }); }
  }

  console.log('');
  console.log(failures ? (failures + ' of ' + checks + ' checks FAILED') : ('OpenPGP module: ' + checks + ' checks passed'));
  process.exit(failures ? 1 : 0);
}()).catch(function (e) {
  console.error('the check threw: ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
