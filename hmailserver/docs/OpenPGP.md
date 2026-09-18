OpenPGP: in the page, and in the Web Key Directory
==================================================

There are two halves to OpenPGP here, and they are worth keeping apart while you
read.

**The directory** publishes users' OpenPGP **public** keys, so that anybody on
the internet who has the address can find the key without asking for it — which
is what a Web Key Directory is, and what stops "I would encrypt to them if I had
their key" being the end of most attempts at encrypted mail. The server holds
only public keys there and does no cryptography with them at all. Nothing about
it is switched on: a server that has never been told otherwise publishes no key
of anybody's, and an upgrade changes nothing about who is published, because
every one of the three switches is off on every row it creates.

**The page** does the cryptography, in the browser. Keys are made or imported
there, kept encrypted under a passphrase this server never sees, and unlocked
for a session; messages are signed, encrypted, decrypted and verified there, in
PGP/MIME, in a module written on the Web Crypto API. The server stores what the
page hands it — including a secret key it cannot open — fetches a
correspondent's key when the browser cannot, and writes the Autocrypt header the
page builds. **This server still signs, verifies, encrypts and decrypts
nothing.**

The format is the whole point. A message this page writes is an ordinary
OpenPGP message: Thunderbird, GnuPG, Mailvelope, Proton, Evolution, KMail and
anything else that speaks the standard read it, and this page reads what they
write. Both directions are proved against GnuPG 2.4 on every pull request
(`build/check-portal-openpgp.js`).

- [OpenPGP in the page](#openpgp-in-the-page)
- [The key page](#the-key-page)
- [Autocrypt](#autocrypt)
- [Finding a correspondent's key](#finding-a-correspondents-key)
- [What the page implements, and what it does not](#what-the-page-implements-and-what-it-does-not)
- [Which browsers](#which-browsers)
- [What a Web Key Directory is](#what-a-web-key-directory-is)
- [Turning it on](#turning-it-on)
- [The DNS and the certificate](#the-dns-and-the-certificate)
- [Putting a key in](#putting-a-key-in)
- [What is refused, and why](#what-is-refused-and-why)
- [What the 404 says](#what-the-404-says)
- [The settings](#the-settings)
- [Where each surface is](#where-each-surface-is)
- [What is stored, and what a backup carries](#what-is-stored-and-what-a-backup-carries)
- [Who may](#who-may)
- [What this does not do](#what-this-does-not-do)

What a Web Key Directory is
---------------------------

`draft-koch-openpgp-webkey-service` — what GnuPG calls WKD, and what
`gpg --locate-keys` and Thunderbird, Enigmail, Evolution, KMail, Mailvelope and
others consult before they ask a key server — is a key published at a URL derived
from the address, over HTTPS.

The address is never in the URL. The local part is lower-cased, hashed with
SHA-1, and encoded in **z-base-32** with the alphabet
`ybndrfg8ejkmcpqxot1uwisza345h769`. The draft's own worked example is
`Joe.Doe` → `iy9q119eutrkn8s1mk4r39qejnbu3n5q`, and that example is a regression
test here (`OpenPgpWebKeyDirectory.TheDraftsOwnExampleIsTheAddress`).

There are two methods and this server serves both:

| Method | The key | The policy file |
|---|---|---|
| **advanced** | `https://openpgpkey.<domain>/.well-known/openpgpkey/<domain>/hu/<hash>` | `https://openpgpkey.<domain>/.well-known/openpgpkey/<domain>/policy` |
| **direct** | `https://<domain>/.well-known/openpgpkey/hu/<hash>` | `https://<domain>/.well-known/openpgpkey/policy` |

Both methods name the domain by its own name - in the path for the advanced
method, in the host for the direct one. A **domain alias** is not resolved: a
request naming an alias is the same 404 as a request naming a domain this server
does not host, so a person whose address is under an alias is found by the
domain's own name and not by the alias.

A client tries the advanced method first. Both take an optional `?l=<local part>`,
which this server accepts and deliberately ignores: the hash is what finds the
key, and two requests for the same hash must not be distinguishable by what they
claim the address was.

The answer is the key as **binary** — not armoured — with
`Content-Type: application/octet-stream` and the
`Access-Control-Allow-Origin: *` the draft requires, so a page in a browser can
fetch it.

The policy file is **empty**, on purpose. Every flag it can carry
(`submission-address`, `mailbox-only`, `dane-only`, `auth-submit`,
`protocol-version`) is about a Web Key *Service* that accepts key submissions by
mail, which this server has none of. A flag here that nothing implements would be
a lie told to every client that reads it.

Turning it on
-------------

Three switches, all off, and **all three have to be on** before one key is
served:

1. **The server.** `OpenPGPWebKeyDirectoryEnabled` in `hm_settings`. Control
   Deck → Settings → OpenPGP key directory; `hmctl settings set openpgp --set
   web_key_directory_enabled=true`; `Settings.OpenPGPWebKeyDirectoryEnabled` over
   COM; `PUT /api/v1/settings/openpgp`.
2. **The domain.** `openpgp_publish` on the domain — Control Deck → Domains →
   Edit → Web Key Directory, `Domain.OpenPGPPublishKeys` over COM, a key of
   `PUT /api/v1/domains/{domain}`.
3. **The account.** `openpgp_publish` on the account. This one is the person's
   own: `PUT /api/v1/me/openpgp/publish` is a self-service route, because holding
   a key so that a mail client can find it is not the same decision as telling
   the whole internet about it. An administrator can set it too
   (`Account.OpenPGPPublish`, a key of `PUT /api/v1/accounts/{address}`).

A fourth, `openpgp_publish_new_accounts` on the domain, decides what an account
**created from now on** starts with. It never touches an account that already
exists, which is what keeps switching it on from publishing a whole domain of
people who did not ask. A restore from backup is excluded from it by name as
well: the archive already says what each account chose.

The listener also has to exist. The directory is served by the **web services
listener**, beside the ACME challenge, the MTA-STS policy and `security.txt`, and
that listener is off until `WebServicesHttpsPort` (or `WebServicesHttpPort`
behind a TLS-terminating proxy) is set.

**Over HTTPS only.** A public key fetched over plain HTTP can be replaced on the
way by anybody on the path, and a replaced key is mail encrypted to somebody
else — the one outcome a key directory exists to prevent. A request that did not
arrive over TLS, and does not carry `X-Forwarded-Proto: https` from a proxy this
server was told to trust (`WebServicesTrustedProxies`, or loopback), gets the
same 404 as an address nobody has, and the reason goes to the debug log.

The DNS and the certificate
---------------------------

For the **advanced** method, which is the one clients try first:

- an `A`/`AAAA` record for `openpgpkey.<domain>` pointing at this server, and
- a certificate that is valid for `openpgpkey.<domain>`.

For the **direct** method, the domain's own web server has to be this one, which
is the less common arrangement.

If you already terminate TLS in front of this server, point
`openpgpkey.<domain>` at the proxy and let it forward `/.well-known/openpgpkey/`
with `X-Forwarded-Proto: https`.

Check it the way a correspondent's client will:

```
gpg --locate-keys --auto-key-locate clear,wkd ann@example.com
curl -v https://openpgpkey.example.com/.well-known/openpgpkey/example.com/hu/<hash>
```

The hash for an address is in every answer this server gives about a key
(`hashed_local_part`), and so are both URLs (`advanced_url`, `direct_url`), so
there is never any need to compute one by hand.

Putting a key in
----------------

The key is the **public** half, exported from wherever the person keeps their
secret half — which is their own machine, and never this server.

```
gpg --armor --export ann@example.com > ann.asc
hmctl openpgp store ann@example.com ann.asc --publish
```

or, without a file:

```
gpg --armor --export ann@example.com | hmctl openpgp store ann@example.com -
```

Over PowerShell:

```powershell
Set-HmOpenPgpKey -Account ann@example.com -Path .\ann.asc -Publish
```

Over REST, by the account itself:

```
PUT /api/v1/me/openpgp          {"key": "<armoured block, or base64 of the bytes>"}
PUT /api/v1/me/openpgp/publish  {"publish": true}
DELETE /api/v1/me/openpgp/{fingerprint}
GET /api/v1/me/openpgp
```

Over REST, by an administrator:

```
GET    /api/v1/accounts/{address}/openpgp
PUT    /api/v1/accounts/{address}/openpgp   {"key": "...", "publish": true}
DELETE /api/v1/accounts/{address}/openpgp/{fingerprint}
GET    /api/v1/domains/{domain}/openpgp
```

Over COM: `Utilities.ImportOpenPGPKey`, `Utilities.RemoveOpenPGPKey`,
`Account.OpenPGPKeys` (read-only, the whole document).

**More than one key an account** is allowed, up to `OpenPGPKeysPerAccount`
(eight). That is not an accident of the schema: a person rotating a key needs the
old one and the new one findable at the same time, or every correspondent who
fetched the old one is writing to a key its owner is about to stop using. The
directory answer is every published key concatenated, in the order they were
uploaded, which is a keyring a client imports whole.

Uploading a key the account **already holds by that fingerprint** replaces the
stored row and answers 200 rather than 201. Re-uploading a key that has gained a
signature is not an error.

What is refused, and why
------------------------

The bytes are read before they are stored. `OpenPgpKey::Parse` walks the packet
framing of **RFC 9580** (the 2024 refresh of RFC 4880) — old-format and
new-format headers, one-, two-, four- and five-octet lengths — and requires the
shape of a transferable public key from section 10.1: a primary public-key
packet, then certifications, user ids, user attributes and subkeys with their
signatures. **The packets have to add up**: the last one ends exactly at the last
byte, or the key is truncated and is refused.

| Refused | Why |
|---|---|
| A **secret**-key packet (tag 5) or secret-subkey packet (tag 7), anywhere | This server never holds a private key. That is the promise of this whole area. |
| An armoured `PGP PRIVATE KEY BLOCK` or `SECRET KEY BLOCK` | Refused by the header line, before a byte of it is decoded. |
| A packet that is not part of a public key, including a **trust packet** | A trust packet belongs to a local keyring and is never part of an export. |
| A second primary key | One upload is one key. A keyring is not a directory answer. |
| A partial or indeterminate body length | That frames a stream of data, never a key. |
| A **version 3** key | Its fingerprint is a hash of the key's numbers rather than of the key itself, and cannot be published this way. Nothing made since 1998 needs one. |
| A key with **no user id** | There is no address it could be published for. |
| A key with **no self-signature** | Nothing in it says the key certified any of its user ids. |
| A key past `OpenPGPKeyMaximumBytes` (64 KiB) | A key that large is a keyring, or a key carrying a very large number of third-party certifications. Export it with its own certifications only. |
| A key whose user ids do not carry **the account's own address** | A key published under an address it does not name is a key every correspondent will encrypt to and its owner cannot read. See the waiver below. |
| A CRC-24 that does not match its armoured data | A truncated paste, which is the commonest way a key arrives broken. |

Every one of those is a whole sentence saying what was wrong and what to do,
wherever the upload came from, because every surface stores through the same
function.

**The fingerprint** is computed the standard way for the key's version
(RFC 9580 section 5.5.4): SHA-1 over `0x99`, the two-octet packet length and the
body for a **version 4** key; SHA-256 over `0x9B` and a four-octet length for a
**version 6** one, which is read as a version 6 key and never refused for being
one; and the LibrePGP `0x9A` form for a version 5 key.

**What is not done, said plainly:** a signature is not verified
cryptographically. "Self-signature" here means a certification whose issuer
fingerprint or issuer key id is this key's own — the packet says this key made
it. Verifying it would mean implementing every signature algorithm OpenPGP has,
and would still not make the key the right one. **What makes a key in this
directory the right one is that its owner or an administrator put it there**, and
nothing more. Do not read more into a stored key than that.

### The address waiver

`allow_address_mismatch` on the administrator's `PUT`, `AllowAddressMismatch` on
`Utilities.ImportOpenPGPKey`, and `--allow-address-mismatch` on
`hmctl openpgp store`, waive **one** rule: that the key's user ids carry the
account's own address. It is a **server administrator's** decision — a credential
confined to domains is refused it — and it exists for a shared or a role address
whose key names the team rather than the mailbox. It is the wrong answer for an
ordinary mailbox, and the account's own route has no such field at all: a person
publishing a key under their own address that names somebody else's is exactly
the mistake the rule catches. `OpenPGPRequireAddressInUserId` turns the rule off
server-wide, which is a blunter instrument and is on by default.

What the 404 says
-----------------

Nothing. Every refusal is the same 404 — the same status, body, content type,
CORS header and `Cache-Control` — whether:

- the domain is not one this server hosts,
- the server switch is off,
- the domain switch is off,
- there is no such account,
- the account exists and has published no key,
- the account exists, has a key, and has not asked to be published,
- the hash is not 32 characters,
- the method is not `GET`,
- or the request did not arrive over HTTPS.

A directory that answered differently for an address nobody has and an address
whose owner has published no key would be a way of asking whether somebody has an
account here, which is what a spammer wants from it. The regression suite
compares two such answers **byte for byte** rather than by status, which is
possible because this server's responses carry no `Date` header.

The settings
------------

All four are rows of `hm_settings` — the database, not `hMailServer.ini` — and
every one is clamped on the way out, so a hand-edited row cannot make the size
cap 0 and refuse every key.

| Setting | REST key | Default | What it is |
|---|---|---|---|
| `OpenPGPWebKeyDirectoryEnabled` | `web_key_directory_enabled` | off | The master switch. With it off nothing is served whatever a domain or an account says. |
| `OpenPGPKeyMaximumBytes` | `key_maximum_bytes` | 65536 | The largest key accepted, in bytes of the key itself rather than of its armour. 1024 to 1048576. |
| `OpenPGPKeysPerAccount` | `keys_per_account` | 8 | How many keys one account may hold at once. 1 to 64. |
| `OpenPGPRequireAddressInUserId` | `require_address_in_user_id` | on | Refuse a key whose user ids do not carry the account's own address. |

Where each surface is
---------------------

| | |
|---|---|
| **Control Deck** | A domain's row → **OpenPGP**: every key held for the domain's accounts, whether each is actually being served, the URL a client fetches, and a form that stores one. Settings → **OpenPGP key directory** for the four above; the domain editor's **Web Key Directory** group and the account editor's **OpenPGP** group for the switches. |
| **REST** | The nine routes listed above, and `GET`/`PUT /api/v1/settings/openpgp`. All of them are in the OpenAPI document the server serves. |
| **COM** | `Settings.OpenPGP*` (4), `Domain.OpenPGPPublishKeys` and `.OpenPGPPublishNewAccounts`, `Account.OpenPGPPublish` and `.OpenPGPKeys`, `Utilities.ImportOpenPGPKey` and `.RemoveOpenPGPKey`. |
| **hmctl** | `openpgp list`, `show`, `store`, `remove` and `publish`, and `settings get openpgp` / `settings set openpgp`. |
| **PowerShell** | `Get-HmOpenPgpKey`, `Set-HmOpenPgpKey`, `Remove-HmOpenPgpKey`, `Set-HmOpenPgpPublish`. |
| **Desktop Control Panel** | Nothing yet. The Deck and the two command-line clients cover it; see [DeckParity.md](DeckParity.md). |

What is stored, and what a backup carries
-----------------------------------------

`hm_pgpkeys` (schema 6050), one row per key: the transferable public key as
base64 of its bytes, the fingerprint, the key version, the algorithm and its
size, the primary user id, when the key was made and when it expires, its size in
bytes, and who uploaded it and when.

It is a per-account store, so it is in the **backup** (`AccountStores`) and a
restore brings it back. Losing it would be quiet, which is exactly why it is
carried: the account would keep reading its mail while every correspondent who
looked the address up got a 404 and fell back to sending in clear. See
[Backup.md](Backup.md).

It is on the **audit trail** as `OpenPGP key`: who put a key under somebody's
address, and who took one away, is the question that matters about a key
directory. An upload the account made itself is recorded under the account; an
administrator's is recorded under the administrator. No row holds a secret, so
nothing in one is redacted. See [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md).


OpenPGP in the page
-------------------

`PortalOpenPgp.js` is served as `/portal-openpgp.js` and is fetched by the
webmail **only when something needs it** \u2014 a message that carries OpenPGP, the
Security page, a send that is to be sealed, or an account that has a key and so
wants an Autocrypt header on what it sends. It is the largest script the page
has, and most sessions never meet an OpenPGP message.

It is written on the Web Crypto API, `BigInt` where Web Crypto stops, and one
small AES of its own, and it vendors nothing: no OpenPGP.js, no library of any
kind. The reasons are written at the top of the file and are worth repeating
here:

- **AES-CFB**, which is what an OpenPGP message body is encrypted with, is not
  in Web Crypto, and each block's input is the previous block's ciphertext, so
  it cannot be batched into one `AES-CTR` call \u2014 it would be one asynchronous
  call per sixteen bytes. So the block cipher is in the file, in about a hundred
  lines. It is a table-free byte-at-a-time implementation and is therefore **not
  constant time**; said plainly, because the alternative is to say nothing. It
  runs in the reader's own browser on the reader's own mail.
- **The AES key wrap** of RFC 3394, which RFC 6637 wraps a session key with, IS
  in Web Crypto (`AES-KW`) and is used from there rather than written again.
- **RSAES-PKCS1-v1_5**, the key transport an RSA subkey uses, is not in Web
  Crypto: the session key goes through `BigInt`, with the private key's CRT
  parameters, exactly as `PortalSmime.js` does it for S/MIME.
- **RSASSA-PKCS1-v1_5** signatures, SHA-1 through SHA-512, PBKDF2 and the random
  numbers all go through Web Crypto, and so do **Ed25519** and **X25519** where
  the browser has them.

The MIME layer is the S/MIME module's. Parsing an entity, decoding a body,
pulling the text, the HTML and the attachments out of one and building one are
done once in this page, in `PortalSmime.js`, and the OpenPGP module calls them.

The key page
------------

Under **Settings \u2192 Security**, beside the S/MIME store.

**Making a key.** A name, an address, a kind \u2014 Curve25519 (the default:
Ed25519 to sign, X25519 to encrypt to) or RSA 3072 or 4096 \u2014 an expiry, and a
passphrase typed twice. Everything happens in the browser. What is stored is the
public key and the secret key **as OpenPGP writes one**: iterated-and-salted
string-to-key, AES-256 in CFB, the SHA-1 checksum of string-to-key usage 254,
under the passphrase. So the bytes this server holds are bytes it cannot open,
and the same bytes saved to a file import into GnuPG.

A **revocation certificate** is made at the same moment and saved to the device.
That is the only time it can be made without the passphrase being typed again,
and it is the one thing a person needs if the key is ever lost. The page says so
when it saves it.

**Importing.** An armoured secret key with its own passphrase is opened and
stored again under the passphrase the page unlocks with; public keys in the same
file, or in a file of their own, are kept as correspondents'. A file holding a
keyring \u2014 several keys one after another \u2014 is read as several keys. A
revocation certificate is recognised as one and applied to the key it revokes.

**What a row shows.** The address, the fingerprint in four-character groups (the
way it is read aloud), the expiry, and, for a key of the account's own, five
things to do with it: copy the public key, save the secret key, publish it in
this server's Web Key Directory, revoke it, remove it. Removing an own key
removes the only copy this server has, and the page says what that means before
it does it.

**Revoking.** The certificate is made by the key itself, applied to it, and the
key stored again with the revocation on it, so that everyone who fetches it
afterwards is told. A revoked key signs nothing and encrypts nothing here and
says why.

**Unlocking.** The passphrase opens the secret keys for the session and is never
sent anywhere. A send that is to be signed stops and asks for it, and sends
nothing until it has it. Signing out forgets the keys, the parsed public keys,
the passphrase boxes and everything else \u2014 a key left in a page after a
sign-out is a key offered to whoever signs in next in that tab.

Autocrypt
---------

[Autocrypt](https://autocrypt.org) Level 1, both ways.

**Outgoing.** A message sent from an address that has a key carries an
`Autocrypt:` header with the minimal form of that key \u2014 the primary key, one
user id with its self-signature, and the living subkeys with their bindings \u2014
and `prefer-encrypt=mutual`. Sealed or in the clear: the header is how a
correspondent's client learns the key without anybody being asked to fetch
anything, and the ordinary case is the message that is not encrypted at all.

The page builds the header, because the page is where the key is. The server
never has to be able to make one and never has to be trusted to. What the server
does do is **refuse** a value that would put something other than one header
line into the message: printable ASCII only, and every CR followed by LF and then
the space or tab that makes it a fold rather than a new header. A header value a
client chose is the shape header injection takes.

**Incoming.** The header is read when a message is opened. If the account has no
key for that sender yet, the key is kept at once, with `autocrypt` recorded as
where it came from, and the reader is told it happened. If the account already
has a **different** key for that address, the new one is **not** kept silently:
it waits behind a button, with both fingerprints in front of the reader, because
a key that changes without anybody being asked is what a man in the middle
needs. (Roadmap2's "Trust made visible" is the row that builds the rest of that.)

Finding a correspondent's key
-----------------------------

`GET /api/v1/me/openpgp/lookup?address=` is the server-side half of finding a
key, because a browser cannot make a cross-origin request to somebody else's
domain and read the answer.

1. **Web Key Directory**, the advanced method first
   (`https://openpgpkey.<domain>/.well-known/openpgpkey/<domain>/hu/<hash>`), then
   the direct one (`https://<domain>/.well-known/openpgpkey/hu/<hash>`). The
   hashed local part is computed exactly as this server computes it when it
   publishes. `openpgp_lookup_wkd`, **on** by default: the request goes to the
   correspondent's own mail domain, which is about to learn of the message anyway.
2. **The keyserver**, `<keyserver_url>/vks/v1/by-email/<address>`.
   `openpgp_lookup_keyserver`, **off** by default and deliberately: a keyserver
   is a third party who is nobody's mail server, and asking it tells it which of
   this server's people are about to write to which address.

Every WKD fetch goes through the client that resolves the name and **refuses
every address inside this network** before a connection is made, caps what it
reads and has a deadline a REST worker can wait on; otherwise anyone with a
mailbox here could aim this server at whatever only this server can reach. The
keyserver is a URL an administrator set, so it is allowed to be an `http`
address on this machine and nothing else in the clear.

**Nothing is stored by the lookup.** It answers the key and where it came from,
the page shows the fingerprint, and the person decides. A key nobody looked at is
not a key anybody has reason to trust.

What the page implements, and what it does not
----------------------------------------------

RFC 4880 version 4, as GnuPG 2.4 emits it by default.

| | |
|---|---|
| **Keys** | Version 4 public and secret keys. RSA 2048/3072/4096; Curve25519 \u2014 Ed25519 signatures (the legacy EdDSA algorithm 22 with the Ed25519Legacy curve, which is what GnuPG writes) and X25519 key agreement as ECDH algorithm 18 with the cv25519 curve, the RFC 6637 KDF and the AES key wrap. |
| **Packets** | Public key (6), public subkey (14), secret key (5), secret subkey (7), signature (2, version 4), one-pass signature (4), user id (13), user attribute (17, kept and passed over), literal data (11), compressed data (8: ZIP and ZLIB read through `DecompressionStream`, nothing written compressed), symmetrically encrypted and integrity protected data (18, version 1, AES-CFB with the SHA-1 modification detection code), symmetrically encrypted data (9, read only, with the resynchronisation that packet alone uses), public-key encrypted session key (1), symmetric-key encrypted session key (3). Partial body lengths, which GnuPG writes whenever it does not know a packet's length in advance. |
| **Signatures** | Made and checked, with the subpackets a key needs: creation, key expiry, key flags, preferred algorithms, features, issuer key id and issuer fingerprint, primary user id, signer user id, revocation reason. **Every self-signature on a key is verified** when the key is read; one that does not verify is not believed. |
| **Messages** | PGP/MIME (RFC 3156): `multipart/signed` with `application/pgp-signature` and `multipart/encrypted` with `application/pgp-encrypted` \u2014 sign, encrypt, sign and encrypt, verify, decrypt. Inline PGP is **read** only: a `BEGIN PGP MESSAGE` or a cleartext-signed block in a text part is opened, and nothing is ever written inline. |
| **Armour** | Radix-64 with its CRC-24, checked; a block whose checksum does not match is refused rather than half-read. |

Not implemented, and refused **by name** where it is met rather than silently
mis-read:

- **Version 5 and version 6 keys and signatures** (RFC 9580 / the librepgp
  draft), and **AEAD** (OCB and EAX). GnuPG 2.4 writes neither by default. Note
  that the directory half of this document reads a version 6 key perfectly well
  \u2014 it only has to compute a fingerprint. The page has to do arithmetic with
  one, and does not.
- **DSA and ElGamal keys**, and the **NIST and Brainpool curves**.
- **3DES, CAST5, IDEA, Blowfish, Twofish, Camellia**, and **BZip2**.
- **Writing** anything compressed, and **writing** inline PGP.
- **Smartcards and hardware tokens** (OpenPGP card, YubiKey). A key here is a key
  in the browser.
- **The web of trust.** Nothing here signs anybody else's key, and no third-party
  certification on a key is read as an opinion. A key is the one the reader
  imported, Autocrypt learned or the directory answered.
- **A draft or a scheduled send that is to be sealed.** The draft route stores
  what it is given and it is given the plaintext; a sealed draft is refused with a
  sentence rather than stored in the clear with a flag the server ignores.

Which browsers
--------------

RSA works everywhere the page works at all. **Curve25519 needs Ed25519 and
X25519 in Web Crypto**, which means Chrome 137, Firefox 130, Safari 17 or later
(and Node 18 or later, which is where the check runs). The key page asks the
browser and says so by name when it has neither; the default kind stays
Curve25519 where it is available, because it is smaller and faster and is what
GnuPG makes by default.

All of it needs a **secure context**: a browser withholds `crypto.subtle` from a
page that was not opened over HTTPS, and the page says that instead of offering
Sign and Encrypt. A reverse proxy that speaks HTTPS to the world and plain HTTP
to this server is the usual way to meet this.

The key ring
------------

`hm_pgpring` (schema 6080), one row per key, per account:

| Kind | What is in it |
|---|---|
| **own** | A key the account made or imported. The public half as base64 of its bytes; the secret half as the **armoured OpenPGP secret key, encrypted under the page's passphrase**. At most 20 an account. |
| **correspondent** | Somebody else's public key, with where it came from (`imported`, `autocrypt`, `wkd`, `keyserver`), when it was first seen and when it was last seen. At most 500 an account. |

Every field of a row \u2014 the fingerprint, the key id, the algorithm, the creation
time, the expiry, and the address when the request named none \u2014 is read out of
the **key** by `OpenPgpKey::Parse`, the same reader the directory publishes
through, and never taken from the request. A row cannot be filed under a
fingerprint that is not its own.

A secret key whose private numbers are **in the clear** is refused rather than
stored (`OpenPgpKey::ReadSecretKey`), and the secret half of a row must be the
same key as the public half beside it. The encryption itself happens where this
server cannot see it, which is exactly why this is the one place that can insist
on it.

The ring is a per-account store, so it is in the **backup** and a restore brings
it back. Losing it would be the worst loss in that file: every message ever
encrypted to the key would become unreadable by anybody, for ever, and nothing
would fail at the moment it happened.

It is **not** on the audit trail. It is the account's own data, written by the
account in its own browser, like its address book and its S/MIME keys.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/openpgp/keys` | The whole ring, the limits, and what lookup the administrator allows |
| `PUT /api/v1/me/openpgp/keys/own` | Add or replace a key of the account's own, secret half and all |
| `DELETE /api/v1/me/openpgp/keys/own/{fingerprint}` | Remove one |
| `PUT /api/v1/me/openpgp/keys/correspondents` | Add or replace a correspondent's public key |
| `DELETE /api/v1/me/openpgp/keys/correspondents/{fingerprint}` | Forget one |
| `GET /api/v1/me/openpgp/lookup?address=` | Fetch a correspondent's key; nothing is stored |
| `GET /portal-openpgp.js` | The module, unauthenticated as the page's other scripts are |

All of them are the **account's own** credentials and nothing else; the
administrative roles do not reach them. `POST /api/v1/me/messages` gains
`autocrypt`, the header value the page built.

The page's own settings, all under `/api/v1/settings/openpgp`:

| Setting | Default | What it decides |
|---|---|---|
| `portal_enabled` | on | Whether the webmail offers any of it. Off, the ring, the lookup and the module are all refused, and the keys already stored are untouched. |
| `lookup_wkd` | on | Look a correspondent's key up in their own domain's directory |
| `lookup_keyserver` | **off** | Ask the keyserver when the directory has none |
| `keyserver_url` | `https://keys.openpgp.org` | Which keyserver. https, or http on this machine. |
| `lookup_timeout` | 10 | Seconds a lookup may take. 2 to 30. |

How it is proved
----------------

`build/check-portal-openpgp.js` runs the module in Node against artefacts GnuPG
2.4 made \u2014 keys on Curve25519 and RSA, detached signatures, a cleartext
signature, messages encrypted with and without a signature, with ZIP, with ZLIB
and with no compression, under AES-128 and AES-256, to one recipient and to two,
a message encrypted to a passphrase, a revoked key, an expired key and two whole
PGP/MIME messages \u2014 and, where it finds `gpg`, hands what the module makes back
to GnuPG: a key it generated, imported both as a public key and as a secret one;
a detached signature; a message encrypted and signed to GnuPG's own key; and a
revocation certificate. Both directions, against an implementation that is not
this one.

The fixtures are committed under `build/fixtures/openpgp/`, so the style workflow
needs no GnuPG at all; `build/make-openpgp-fixtures.sh` remakes them where GnuPG
2.4 is. **The secret keys in that directory are test keys and their passphrase is
in the README beside them**; their addresses are in the `.test` top-level domain,
which can never be delivered to.

`API/RestApiOpenPgpKeys` proves the server's half \u2014 the store, the refusals, the
lookup against a keyserver the fixture serves itself, and the Autocrypt header on
a message \u2014 and `Infrastructure/BackupAccountStores` proves the ring comes back
from an archive with its secret key byte for byte.

Who may
-------

| Doing this | Needs |
|---|---|
| Reading the settings, an account's keys or a domain's listing | `view_configuration` (in that domain, for the last two) |
| Storing or removing a key for an account | `manage_accounts` **in that domain** |
| Changing the four server settings | `manage_settings`, and not a credential confined to domains |
| Waiving the address rule for one upload | The server administrator |
| An account's own keys and its own publish switch | The account's own credential (self-service; the roles do not reach it) |
| The webmail's key ring, and the lookup | The account's own credential; the roles do not reach it |
| The webmail's five OpenPGP settings | `manage_settings`, and not a credential confined to domains |

The auditor role reads all of it and changes none of it. See [Roles.md](Roles.md).

What this does not do
---------------------

- **No encryption or decryption on the server, anywhere.** Nothing in this
  server signs, verifies, encrypts or decrypts an OpenPGP message. All of that
  is in the page, in the browser, under a key this server cannot open — see the
  first half of this document.
- **No key submission by mail.** There is no Web Key *Service*: a key arrives
  through an administrator, a command-line client or the account's own route, and
  not by sending mail to `key-submission@`. The policy file says so by carrying
  no `submission-address`.
- **No key server in the publishing direction.** Nothing is ever uploaded to
  `keys.openpgp.org` or any other pool. The webmail can FETCH a correspondent's
  key from one, when an administrator turns that on; it is off by default and
  nothing ever goes the other way.
- **No filtering of what is served.** The draft suggests a published key may be
  stripped of user ids other than the one being looked up. This server serves the
  bytes that were uploaded, unchanged: a key it cannot re-sign is a key it must
  not rewrite.
- **No DANE (`OPENPGPKEY` records).** Publishing a key in DNS is a different
  mechanism with a different set of clients, and is not here.
- **No revocation handling in the directory.** A revoked key stays published
  until somebody removes it. Remove it. (The page is a different matter: it reads
  a revocation, applies it to the key, refuses to sign or encrypt with a revoked
  key, and stores the key again with the revocation on it, so that a directory
  serving that key serves the revocation with it.)
