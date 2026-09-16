OpenPGP keys, and the Web Key Directory
=======================================

This server can publish its users' OpenPGP **public** keys, so that anybody on
the internet who has the address can find the key without asking for it — which
is what a Web Key Directory is, and what stops "I would encrypt to them if I had
their key" being the end of most attempts at encrypted mail.

It holds **only public keys**, and it never does any cryptography with them. It
does not sign, verify, encrypt or decrypt anything. That is deliberate and it is
not a gap that will be filled here: the encryption and decryption belong in the
webmail page, in the browser, under a key the server cannot read — the same shape
the S/MIME support already has (`PortalSmime.js`). What is here is the directory.

Nothing is switched on. A server that has never been told otherwise publishes no
key of anybody's, and an upgrade to the release that introduced this changes
nothing about who is published, because every one of the three switches is off on
every row it creates.

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

Who may
-------

| Doing this | Needs |
|---|---|
| Reading the settings, an account's keys or a domain's listing | `view_configuration` (in that domain, for the last two) |
| Storing or removing a key for an account | `manage_accounts` **in that domain** |
| Changing the four server settings | `manage_settings`, and not a credential confined to domains |
| Waiving the address rule for one upload | The server administrator |
| An account's own keys and its own publish switch | The account's own credential (self-service; the roles do not reach it) |

The auditor role reads all of it and changes none of it. See [Roles.md](Roles.md).

What this does not do
---------------------

- **No encryption or decryption, anywhere.** Nothing in this server signs,
  verifies, encrypts or decrypts an OpenPGP message. That belongs in the page,
  in the browser, and is a separate row of the roadmap.
- **No key submission by mail.** There is no Web Key *Service*: a key arrives
  through an administrator, a command-line client or the account's own route, and
  not by sending mail to `key-submission@`. The policy file says so by carrying
  no `submission-address`.
- **No key server.** Nothing is uploaded to or fetched from `keys.openpgp.org` or
  any other pool, in either direction.
- **No filtering of what is served.** The draft suggests a published key may be
  stripped of user ids other than the one being looked up. This server serves the
  bytes that were uploaded, unchanged: a key it cannot re-sign is a key it must
  not rewrite.
- **No DANE (`OPENPGPKEY` records).** Publishing a key in DNS is a different
  mechanism with a different set of clients, and is not here.
- **No revocation handling.** A revoked key stays published until somebody
  removes it. Remove it.
