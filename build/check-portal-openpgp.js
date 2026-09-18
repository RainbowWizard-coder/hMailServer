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

      const symmetricMine = await pgp.encryptBytes({ data: mine, recipients: [], passwords: ['page-symmetric'] });
      const symmetricThere = gpg(home, ['--decrypt'], Buffer.from(pgp.armor('MESSAGE', symmetricMine), 'utf8'), 'page-symmetric');
      check(symmetricThere.ok && symmetricThere.text.indexOf('What the page wrote.') >= 0, 'GnuPG opens the page\'s passphrase-encrypted message');
    }
    finally { fs.rmSync(home, { recursive: true, force: true }); }
  }

  console.log('');
  console.log(failures ? (failures + ' of ' + checks + ' checks FAILED') : ('OpenPGP module: ' + checks + ' checks passed'));
  process.exit(failures ? 1 : 0);
}()).catch(function (e) {
  console.error('the check threw: ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
