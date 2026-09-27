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
- [A password-protected message to anyone](#a-password-protected-message-to-anyone)
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
   COM; `PUT /api/v1/settings/openpgp`; the Control Panel's Webmail page, the
   Encryption tab.
2. **The domain.** `openpgp_publish` on the domain — Control Deck → Domains →
   Edit → Web Key Directory, `Domain.OpenPGPPublishKeys` over COM, a key of
   `PUT /api/v1/domains/{domain}`, the Control Panel's domain dialog, the
   OpenPGP tab.
3. **The account.** `openpgp_publish` on the account. This one is the person's
   own: `PUT /api/v1/me/openpgp/publish` is a self-service route, because holding
   a key so that a mail client can find it is not the same decision as telling
   the whole internet about it. An administrator can set it too
   (`Account.OpenPGPPublish`, a key of `PUT /api/v1/accounts/{address}`, the
   Control Panel's account dialog on its Identity tab).

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

The three writes are the account holder's alone: a request made with an app password,
or in an administrator's support session, reads the document and is refused the
writes with 403 (`code` `support_session` for the latter). The key published here is
the one every correspondent who looks the address up encrypts to, so a credential that
could replace it could read what they send. An administrator who keeps a person's key
for them uses the administrator's routes below, where the change is theirs on the
record.

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
nothing more. Do not read more into a stored key than that. The one signature on a
key the server does check is a subkey's binding, where it encrypts to the key - a
zero-access mailbox's own key ([ZeroAccess.md](ZeroAccess.md)) and a partner's gateway
key ([RemoteDomains.md](RemoteDomains.md)): a subkey is the part of a key a stranger
can append without changing its fingerprint, so only one whose binding verifies
against the primary is encrypted to.

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
| **Desktop Control Panel** | Over COM: the four settings above on Connections & protocols > Webmail, the **Encryption** tab's OpenPGP card; the domain dialog's **OpenPGP** tab for `Domain.OpenPGPPublishKeys` and `.OpenPGPPublishNewAccounts`; the account dialog's **Identity** tab for `Account.OpenPGPPublish`. Not the keys themselves: listing, storing and removing one is the Deck's and the two command-line clients'; see [DeckParity.md](DeckParity.md). |

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
webmail **only when something needs it** — a message that carries OpenPGP, the
Security page, a send that is to be sealed, or an account that has a key and so
wants an Autocrypt header on what it sends. It is the largest script the page
has, and most sessions never meet an OpenPGP message.

It is written on the Web Crypto API, `BigInt` where Web Crypto stops, and one
small AES of its own, and it vendors nothing: no OpenPGP.js, no library of any
kind. The reasons are written at the top of the file and are worth repeating
here:

- **AES-CFB**, which is what an OpenPGP message body is encrypted with, is not
  in Web Crypto, and each block's input is the previous block's ciphertext, so
  it cannot be batched into one `AES-CTR` call — it would be one asynchronous
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

S/MIME, the other end-to-end format the page speaks, keeps its keys in a store
of its own beside this ring, the private key wrapped under the account password
in the same way. A domain may also have its **own S/MIME authority**, which
certifies the keys its accounts make in this page, so that colleagues sign and
encrypt to each other without anybody buying a certificate: the key is made in
the browser and never sent, the authority only signs the request, and a
colleague's certificate is looked up by address when a message to them is to be
encrypted. That is [SmimeAuthority.md](SmimeAuthority.md). OpenPGP has no
counterpart here, because its trust rests on the fingerprints people check with
each other, not on an authority the server runs.

The key page
------------

Under **Settings → Security**, beside the S/MIME store.

**Making a key.** A name, an address, a kind — Curve25519 (the default:
Ed25519 to sign, X25519 to encrypt to) or RSA 3072 or 4096 — an expiry, and a
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
keyring — several keys one after another — is read as several keys. A
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
the passphrase boxes and everything else — a key left in a page after a
sign-out is a key offered to whoever signs in next in that tab.

Autocrypt
---------

[Autocrypt](https://autocrypt.org) Level 1, both ways.

**Outgoing.** A message sent from an address that has a key carries an
`Autocrypt:` header with the minimal form of that key — the primary key, one
user id with its self-signature, and the living subkeys with their bindings —
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
needs. Kept is not accepted: the server files it as a change of that address's
key, and what happens next is the section below.

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

An address that is not at a domain name has no directory, and none is asked:
an address literal (`someone@[192.0.2.1]`), a name of one label, and a name
whose last label begins with a digit - an IPv4 address written as a name, in any
of the forms a resolver reads as one (`198.51.100.7`, `0xc6.0x33.0x64.0x7`),
since no top-level domain begins with one. The address validator admits all
of them, the direct leg would connect to the address itself, and a caller has a
new one for every lookup with no organization to hold it by. The lookup says so
and goes on to the keyserver if that is on (`OpenPgpRing::MayHaveWebKeyDirectory`).

A domain whose key directory took three seconds or more to ask - both addresses
together, whatever they answered: nothing, a 404, or even the key - is held
against its whole organizational domain (the Public Suffix List's answer) for an
hour: a lookup of any address under it does not ask the directory, says so, and
goes on to the keyserver if that is on. Whoever owns a domain has a new
subdomain, or a new local part, for every lookup, and runs the directory each
one is asked at; each would otherwise hold a REST worker for up to the whole
`lookup_timeout`. Silence alone is not the test, because a 404 or a key sent
after a wait costs the worker the same and is as easy to send. A directory that
answers at once, whatever it answers, or a host that refuses at once, is not
held. The cost falls on a directory that is slow for everybody: once it has
taken three seconds, the next address at its domain is not looked up there for
an hour (`OpenPgpRing::NoteDirectoryUnreachable`).

**Nothing is stored by the lookup.** It answers the key and where it came from,
the page shows the fingerprint, and the person decides. A key nobody looked at is
not a key anybody has reason to trust.

Trust made visible
------------------

The one thing a man in the middle needs is silence: a key that changes without
anybody being told. So the ring remembers, and the page says.

**First seen is remembered.** A correspondent's key is filed with the moment it
was first seen (`first_seen`), and the same key seen again - by a later lookup,
a later message, a later import - keeps that moment, whatever brought it back.
It keeps its **address** the same way: a key that comes back with another
address in the request stays filed where it was, because a row that moved would
carry its mark and its history to an address they were never about. A person
who wants a key under another address removes it and imports it again.

**A change is a warning, never a silent switch.** A DIFFERENT key arriving for
an address the ring already holds a key for - a lookup that answered another
fingerprint, a key kept from a message's Autocrypt header, an import over the
one held - is stored as a **change**: the new row carries `changed_from` (the
fingerprint it replaced), `changed_at` (when it arrived) and `accepted`
(false). Until the person accepts it, the page does **not** encrypt to it - a
send to that address stops and says why - and a signature it made, however good
the arithmetic, is shown with the warning and nothing green:

> This correspondent's key changed on 18 September 2026. It was 34B2 AC7E ...;
> it is now C29B 6CCE ...

The same sentence stands on the keys page, on the row, with **Accept the new
key** beside it, and in the reader, with the same button. Accepting is
`PUT /api/v1/me/openpgp/keys/{id}/accepted` with `{"accepted": true}`; the
change stays on record afterwards, so the row can still say what it replaced.
The key it replaced is untouched - still there, still usable, still whatever
the person said it was - and can be removed by hand.

Which key a new arrival is a change *of*: the address's newest **settled** key,
one that replaced nothing or whose replacement was accepted; a pending key
counts only when no settled one is left, so that removing the old key does not
make the next arrival look like the first. **The account's own key counts**,
above all: a correspondent's key arriving for an address the account holds its
own key for is a change of the own key. The page seals every message to the
sender's own key so that the copy in Sent can be read, and a stranger's key
filed under that address in silence - a spoofed `From: me` with an Autocrypt
header, or a key whose user id carries no address at all - would have every
message the person sends sealed to the stranger. So the page never fills the
account's own address in for a correspondent's key, the server refuses a
correspondent's key it cannot file under an address, and a key that does
arrive for the account's address waits behind Accept like any other change.

When a settled key and a pending one coexist for an address, a message is
sealed to the settled one and the compose form says that a newer key is waiting.

**Fingerprints shown, verifiable in person.** Every key's fingerprint is on its
row in groups of four hexadecimal characters, the way it is read aloud, the
person's own included; a good signature's is in the reader below the verdict. A
correspondent's key can be marked **verified in person** - `PUT
/api/v1/me/openpgp/keys/{id}/verified` with `{"verified": true}`, and `false`
to unmark - which is the account's own word that the fingerprint was read back
to the key's owner, by voice or face to face. The mark is stored per key in the
ring (`verified`, `verified_at`); it is refused to a request authenticated with
an app password, because an app password is a credential for a program that
reads and sends mail and a program has no way of knowing such a thing; it is
404 for a row that is not this account's and 400 for a key of the account's
own, which is nobody's correspondent. Marking a key that replaced another
accepts the change in the same act: a fingerprint read back to its owner is the
stronger of the two.

**A green mark from then on.** A message whose signature is good AND by a
verified key shows the mark beside the sender in the reader (a green badge with
a tick, "Signed by ... , verified in person"); a good signature by an unverified
key shows the plain verdict and the fingerprint; a good signature by a key that
changed and was not accepted shows the warning. The keys page shows the same
three states on the rows. **The mark is never inherited**: a different key
arriving for a verified correspondent's address is a new row with no mark, so a
new key is not verified because the old one was.

The server acts on none of this. It records what the account said and what the
ring noticed, and answers it in every key's JSON: `verified` and `verified_at`
on every row, and `changed_from`, `changed_at` and `accepted` on a row that
replaced another (absent means "replaced nothing"). The page decides what to
encrypt to and what to call good, as it always has.
A password-protected message to anyone
--------------------------------------

For the correspondent who has no key. The sender ticks **Protect with a
password** on the compose form, types a password twice - the form says in a
sentence how strong it is, and never sends it anywhere - and, if they like, a
hint. On Send the page builds the text and the attachments into one MIME
message and encrypts that message to the password as **one OpenPGP message**:
a symmetric-key encrypted session key and an integrity-protected body, RFC
4880 sections 5.3 and 5.13, the shape `gpg -c` writes and reads. The armour
goes to this server in chunks (`POST /api/v1/me/protected`, then `PUT
/api/v1/me/protected/{id}/content?offset=`, as a file sent as a link goes),
and this server files it under `<DataDirectory>/Protected/<account id>/<token>.asc`
with a row in `hm_protectedmessages` (schema 6082).

What goes out by SMTP is a **short notice** from the sender - a sentence saying
a password-protected message is waiting, the hint if one was given, and the
link `https://<this server>/portal/protected/<token>` - written by this
server, which is how it can say what crossed the internet. A copy of the
notice goes to the sender's Sent folder, with the token in an
`X-hMailServer-Protected` header, and the reader shows beside it whether the
link has been opened.

**The reading page** (`/portal/protected/<token>`) takes no session and no
account: it fetches the ciphertext, the hint and the dates from
`GET /api/v1/protected/<token>`, asks for the password, decrypts in the
browser with the page's own OpenPGP module, shows the text and offers each
attachment to save. A wrong password says "That password is not right" and
nothing else, because a wrong password and a damaged message are not the
reader's to tell apart. The page is plain, works on a phone, and is served
with the same headers as the webmail - nothing runs but this server's scripts
and nothing is fetched from anywhere else.

**A reply goes back the same way.** The reader writes text on that page, the
page encrypts it under the same password and posts it to
`POST /api/v1/protected/<token>/reply`; this server files it under the
original's sender, with the original's token on the row, and sends the sender
a short internal message from the domain's postmaster with a link to
`/portal/protected/<reply token>`. The password the sender set opens it. A
reply cannot be replied to, and an original may collect at most twenty.

**The link is the whole access control.** The token is 128 bits from the
system's random source, written as 22 characters of base64url, and the fetch
takes no credential: whoever holds the link fetches the ciphertext, and
whoever holds the password reads it. That is what a capability link is. The
fetch and the reply are rate-limited per address (a hundred and twenty an
hour; loopback exempt), a token that names nothing is answered 404 without
saying whether one ever existed, and a withdrawn or expired link answers a
plain sentence. A stale token counts for nothing else, deliberately: a page
fetches its link on load, with no human action, and a link older than its
row is exactly such a token - counting it as a failed logon would ban the
reader of an old link from the whole server.

**What it promises, and all it promises.** The plaintext is never in the
message that crosses the internet and never rests on this server. Three
things make that true. The page encrypts before it sends. No attachment is
sent as a link while a message is protected, whatever the domain's link
threshold says - a file sent as a link rests on this server in the clear
behind a public link, so under Protect every file goes into the ciphertext,
and a message that already carries a link from earlier in the same compose
is refused rather than sealed around a hole. And this server files only what
has the **shape** of an OpenPGP message to a passphrase:
`ProtectedMessages::IsSymmetricMessage` reads the armour, checks its CRC and
walks the packets, which have to be one or more symmetric-key encrypted
session keys (each a few dozen bytes) and then exactly one integrity-protected
data packet running to the end - a public-key session key, a literal packet
slipped in between, an unprotected data packet or anything after the body is
refused by name, and the record discarded with the reason. What this server
cannot tell is ciphertext from noise, and it does not claim to; what it can
tell is either of those from a plaintext, and that is the promise. It does
**not** promise that the recipient cannot copy or forward what they read: a
page that has shown somebody a text has shown it to them. The compose form
and the reading page both say so in one sentence. And the sender's own Sent
folder holds the notice alone: their text is readable only through the link,
with the password, until it expires - the compose form says that too. This is
the honest version of "confidential mode": it promises only what it keeps.

**Opened** is when the ciphertext was first fetched, which is all this server
can know; whether the password was ever typed happens in the reader's browser.

The ceilings, each refused with a sentence: 25 MB a message (text and
attachments together, as ciphertext), 250 MB and a thousand live messages an
account - counted from the declared size, so a thousand declared uploads
cannot outrun them - and 1 MB for a reply. A chunk for an offset the file has
already passed is refused with where the file really is, judged under the
lock the append takes. A record never finished goes after
a day; the ciphertext goes when the message expires or is withdrawn, and the
row thirty days after that, so the sender's list still says what became of it.
The sweep is the scheduled-mail task's, every minute, and the retention task's
on demand.

A message lives `protected_message_days` (30 by default, 1 to 365, under
`/api/v1/settings/openpgp`): the default a sender gets and the most a sender
may ask for (`expires_in`). The feature needs OpenPGP on in the webmail
(`portal_enabled`) and a public address for the links - the same
`confidential_base_url` chain the messages that expire use - and is offered
only on a page opened over HTTPS, as Sign and Encrypt are. A request
authenticated with an app password can neither make one, upload one,
withdraw one nor list them: the list carries every live link and hint, which
is more than a program that reads mail needs. A message to a password
cannot be a draft or a scheduled send, since the draft route is given the
plaintext; nor is it also signed or encrypted to a key.

| Route | What it does |
|---|---|
| `GET /api/v1/me/protected` | Every one sent, newest first, with the limits and what is used |
| `POST /api/v1/me/protected` | The record: recipient, hint, size, expires_in |
| `PUT /api/v1/me/protected/{id}/content?offset=` | The ciphertext, in chunks; the notice goes when the last one lands |
| `DELETE /api/v1/me/protected/{id}` | Withdraw one |
| `GET /api/v1/protected/{token}` | The ciphertext, to anyone with the link (no credential) |
| `POST /api/v1/protected/{token}/reply` | A reply, encrypted under the same password (no credential) |
| `GET /portal/protected/{token}` | The reading page; `/portal-protected.js` its script |

`hm_protectedmessages` is not in the backup, and **Back up messages** leaves
`<data directory>\Protected`, where the ciphertext is, out of its copy of the
data directory: the ciphertext is behind links that name this server and end on
a timer, a restore elsewhere could not serve them, the sender's copy of the
notice is in the Sent folder the backup does carry, and what the recipient read
is theirs. A restore leaves that folder where it is. It is not on the audit trail:
it is traffic, and ciphertext this server cannot open. An address's erasure
([PersonalData.md](PersonalData.md)) ends every message to the address and every
reply it wrote - the ciphertext deleted, the row gone - unless the sending
account is under a legal hold. A message the address's own account sent is not
reached: its row names its recipient and no sender, and goes with the account.

`API/RestApiProtectedMessages` proves the server's half - the bytes stored are
the ciphertext, what does not have the shape is discarded (a plaintext, a
message to a key, a literal packet between the session key and the body, a
packet after it), the link's shape, the fetch with no credential, a stale
token fetched four times leaving the address able to sign in, the notice with
the link and the hint and nothing of the text, the withdrawal, the expiry, the
never-finished record and the orphaned directory swept, the ceilings, the
second chunk for one offset, the reply filed under the sender, the reply
ceilings, the app password refused on every route - and
`build/check-portal-openpgp.js` proves the page's: the module's message opens
with the password and not without, a message that was never encrypted or was
encrypted without integrity protection is refused as a wrong password would
be, GnuPG opens the page's message with `gpg --decrypt` and the page opens what
`gpg -c` made. The page harness runs the reading page itself, with the real
modules, against a message the real module sealed, and holds the compose form
to sending no file as a link while Protect is ticked.

What the page implements, and what it does not
----------------------------------------------

RFC 4880 version 4, as GnuPG 2.4 emits it by default.

| | |
|---|---|
| **Keys** | Version 4 public and secret keys. RSA 2048/3072/4096; Curve25519 — Ed25519 signatures (the legacy EdDSA algorithm 22 with the Ed25519Legacy curve, which is what GnuPG writes) and X25519 key agreement as ECDH algorithm 18 with the cv25519 curve, the RFC 6637 KDF and the AES key wrap. |
| **Packets** | Public key (6), public subkey (14), secret key (5), secret subkey (7), signature (2, version 4), one-pass signature (4), user id (13), user attribute (17, kept and passed over), literal data (11), compressed data (8: ZIP and ZLIB read through `DecompressionStream`, nothing written compressed), symmetrically encrypted and integrity protected data (18, version 1, AES-CFB with the SHA-1 modification detection code), symmetrically encrypted data (9, read only, with the resynchronisation that packet alone uses), public-key encrypted session key (1), symmetric-key encrypted session key (3). Partial body lengths, which GnuPG writes whenever it does not know a packet's length in advance. |
| **Signatures** | Made and checked, with the subpackets a key needs: creation, key expiry, key flags, preferred algorithms, features, issuer key id and issuer fingerprint, primary user id, signer user id, revocation reason. **Every self-signature on a key is verified** when the key is read; one that does not verify is not believed. |
| **Messages** | PGP/MIME (RFC 3156): `multipart/signed` with `application/pgp-signature` and `multipart/encrypted` with `application/pgp-encrypted` — sign, encrypt, sign and encrypt, verify, decrypt. Inline PGP is **read** only: a `BEGIN PGP MESSAGE` or a cleartext-signed block in a text part is opened, and nothing is ever written inline. |
| **Armour** | Radix-64 with its CRC-24, checked; a block whose checksum does not match is refused rather than half-read. |

Not implemented, and refused **by name** where it is met rather than silently
mis-read:

- **Version 5 and version 6 keys and signatures** (RFC 9580 / the librepgp
  draft), and **AEAD** (OCB and EAX). GnuPG 2.4 writes neither by default. Note
  that the directory half of this document reads a version 6 key perfectly well
  — it only has to compute a fingerprint. The page has to do arithmetic with
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

`hm_pgpring` (schema 6080; the trust columns are 6081), one row per key, per
account. A zero-access mailbox ([ZeroAccess.md](ZeroAccess.md)) stands on one of
the account's own keys here: every delivery is encrypted to it by the server, and
two more copies of its secret half - one sealed under a passphrase derived from the
account password, one under a recovery key - live in `hm_zeroaccess` (schema 6083),
which is why that key cannot be removed from the ring while the switch is on.

| Kind | What is in it |
|---|---|
| **own** | A key the account made or imported. The public half as base64 of its bytes; the secret half as the **armoured OpenPGP secret key, encrypted under the page's passphrase**. At most 20 an account. |
| **correspondent** | Somebody else's public key, with where it came from (`imported`, `autocrypt`, `wkd`, `keyserver`), when it was first seen and when it was last seen. At most 500 an account. |

Every field of a row — the fingerprint, the key id, the algorithm, the creation
time, the expiry, and the address when the request named none — is read out of
the **key** by `OpenPgpKey::Parse`, the same reader the directory publishes
through, and never taken from the request. A row cannot be filed under a
fingerprint that is not its own.

A secret key whose private numbers are **in the clear** is refused rather than
stored (`OpenPgpKey::ReadSecretKey`), and the secret half of a row must be the
same key as the public half beside it. The encryption itself happens where this
server cannot see it, which is exactly why this is the one place that can insist
on it.

Five columns are the ring's own and never the request's: `pgrverified` and
`pgrverifiedat` (the account's "verified in person" mark and when it was set),
`pgrchangedfrom` and `pgrchangedat` (the fingerprint a correspondent's key
replaced for its address, and when it arrived) and `pgraccepted` (whether the
person accepted that change). A row that comes back keeps them as they were; a
new row has none; a correspondent's key filed under an address that already
holds a different key gets its change written by `OpenPgpRing::Save` itself.

The ring is a per-account store, so it is in the **backup** and a restore brings
it back. Losing it would be the worst loss in that file: every message ever
encrypted to the key would become unreadable by anybody, for ever, and nothing
would fail at the moment it happened.

It is **not** on the audit trail. It is the account's own data, written by the
account in its own browser, like its address book and its S/MIME keys.

A correspondent's key names its owner in its user id, so an address's erasure
([PersonalData.md](PersonalData.md)) removes the correspondent rows filed under
the address from every account's ring, as it removes the certificates other
accounts keep of it. An account's own keys go with the account.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/me/openpgp/keys` | The whole ring, the limits, and what lookup the administrator allows. To an app password or an administrator's support session, own keys without their `secret_key` (`secret_key_withheld`): the encrypted secret key is an offline test of a guess at its passphrase |
| `PUT /api/v1/me/openpgp/keys/own` | Add or replace a key of the account's own, secret half and all |
| `DELETE /api/v1/me/openpgp/keys/own/{fingerprint}` | Remove one |
| `PUT /api/v1/me/openpgp/keys/correspondents` | Add or replace a correspondent's public key |
| `DELETE /api/v1/me/openpgp/keys/correspondents/{fingerprint}` | Forget one |
| `PUT /api/v1/me/openpgp/keys/{id}/verified` | Mark a correspondent's key verified in person, or unmark it (`verified`: true or false) |
| `PUT /api/v1/me/openpgp/keys/{id}/accepted` | Accept that a correspondent's key replaced the one held before it, or withdraw that (`accepted`: true or false) |
| `GET /api/v1/me/openpgp/lookup?address=` | Fetch a correspondent's key; nothing is stored |
| `GET /portal-openpgp.js` | The module, unauthenticated as the page's other scripts are |

All of them are the **account's own** credentials and nothing else; the
administrative roles do not reach them. Every **write** to the ring - the four
that store or remove a key, and the two trust routes - is refused to a request
authenticated with an app password, and to an administrator's support session
(403 with `code` `support_session`). An app password is a program's credential,
and a support session is an administrator helping the person, who must not change
what their mail is sealed to or what they have said about a correspondent's key: the
ring
holds the account's secret key and decides what its mail is sealed to, and with
an app password one could forget a settled key and a pending one and store the
key again with no change on it, forget a verified key and store it again with no
mark, or remove the only copy of the secret key. Reading the ring is not
refused, since a program that reads mail may need the keys. The S/MIME store's
four writes (`PUT` and `DELETE` of `/api/v1/me/smime/own` and
`/api/v1/me/smime/recipients`) are refused to both in the same way. `POST
/api/v1/me/messages` gains `autocrypt`, the header value the page built.

The page's own settings, all under `/api/v1/settings/openpgp`, and on the Control
Panel's Webmail page, the Encryption tab, which reads and writes them over the REST
API, so the REST API has to be on:

| Setting | Default | What it decides |
|---|---|---|
| `portal_enabled` | on | Whether the webmail offers any of it. Off, the ring, the lookup and the module are all refused, and the keys already stored are untouched. |
| `lookup_wkd` | on | Look a correspondent's key up in their own domain's directory |
| `lookup_keyserver` | **off** | Ask the keyserver when the directory has none |
| `keyserver_url` | `https://keys.openpgp.org` | Which keyserver. https, or http on this machine. |
| `lookup_timeout` | 10 | Seconds a lookup may take. 2 to 30. |
| `protected_message_days` | 30 | How many days a password-protected message is kept for its reading page. 1 to 365. |

How it is proved
----------------

`build/check-portal-openpgp.js` runs the module in Node against artefacts GnuPG
2.4 made — keys on Curve25519 and RSA, detached signatures, a cleartext
signature, messages encrypted with and without a signature, with ZIP, with ZLIB
and with no compression, under AES-128 and AES-256, to one recipient and to two,
a message encrypted to a passphrase, a revoked key, an expired key and two whole
PGP/MIME messages — and, where it finds `gpg`, hands what the module makes back
to GnuPG: a key it generated, imported both as a public key and as a secret one;
a detached signature; a message encrypted and signed to GnuPG's own key; and a
revocation certificate. Both directions, against an implementation that is not
this one.

The fixtures are committed under `build/fixtures/openpgp/`, so the check needs no
GnuPG at all; GitLab CI's harnesses job installs GnuPG, so it runs the other half
too. `build/make-openpgp-fixtures.sh` remakes the fixtures where GnuPG 2.4 is.
**The secret keys in that directory are test keys and their passphrase is in the
README beside them**; their addresses are in the `.test` top-level domain,
which can never be delivered to.

`API/RestApiOpenPgpKeys` proves the server's half — the store, the refusals, the
lookup against a keyserver the fixture serves itself, and the Autocrypt header on
a message — and `Infrastructure/BackupAccountStores` proves the ring comes back
from an archive with its secret key byte for byte. Three tests of the same
fixture prove the trust half: the first key seen is remembered and a different
one for the address is filed as a change and stays unaccepted until accepted;
the mark needs the account's own session (an app password is refused and
changes nothing, another account finds no such key, an own key has nothing to
verify); and the mark is not inherited by the key that replaced a verified one,
while verifying that key accepts the change. Three more, from the row's review:
a key for the account's own address is a change of the own key, and a key with
no address is refused; a key comes back to the address it was filed under; and
a pending key counts when no settled one is left. The four ring writes on an app
password are refused in the mark's test. The page's half - the three states on
the rows and in the reader, the warning naming both fingerprints, the send that
stops, Accept and the mark from the list and from the reader, a key never filed
under the account's own address in silence, the auto-learned change said with
both fingerprints, and the newer-key-waiting hint - is twenty-three checks in
`build/portal-script-test.js`.
from an archive with its secret key byte for byte.

Who may
-------

| Doing this | Needs |
|---|---|
| Reading the settings, an account's keys or a domain's listing | `view_configuration` (in that domain, for the last two) |
| Storing or removing a key for an account | `manage_accounts` **in that domain** |
| Changing the four server settings | `manage_settings`, and not a credential confined to domains |
| Waiving the address rule for one upload | The server administrator |
| An account's own keys and its own publish switch | The account's own credential (self-service; the roles do not reach it) - never an app password nor an administrator's support session |
| Reading the webmail's key ring, and the lookup | The account's own credential; the roles do not reach it |
| Storing or removing a key in the ring, marking one verified in person, or accepting a change of key | The account's own session - never an app password, which is a program's, nor an administrator's support session |
| The webmail's five OpenPGP settings | `manage_settings`, and not a credential confined to domains |
| The webmail's key ring, and the lookup | The account's own credential; the roles do not reach it |
| The webmail's six OpenPGP settings | `manage_settings`, and not a credential confined to domains |
| A password-protected message: sending one, listing them, seeing whether one was opened, withdrawing it | The account's own credential, and never an app password |
| Reading one, and replying on its page | Whoever holds the link, and the password; no credential at all |

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
