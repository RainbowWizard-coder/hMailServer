An organisation CA for S/MIME
=============================

S/MIME needs a certificate for every person who signs or is written to, and a
certificate is normally bought, one person at a time, from a public authority.
Inside one organisation that is the wrong shape: Exchange with Active Directory
Certificate Services gives every mailbox a certificate from the organisation's
own authority, published for colleagues to look up, and nobody buys anything.

This server does the same, one domain at a time. A domain may have its own
**S/MIME authority**: a root certificate the domain owns, which certifies the
keys its accounts make in the webmail, publishes what it certified for the
domain's own people to look up, and says what it has revoked in a signed CRL.
The code is `Common/Util/OrganisationCa.cpp` (the authority, the signing, the
CRL and the backup), `Common/Util/RestApiSmimeAuthority.cpp` (the routes),
`WebServicesServer::HandleSmimeAuthority_` (the public half) and
`PortalSmime.js`'s `makeRequest` (the request the webmail makes).

Nothing is issued until an administrator creates a domain's authority. A server
with none - which is every server after the upgrade - does exactly what it did.

- [Who holds what](#who-holds-what)
- [Creating the authority](#creating-the-authority)
- [Enrolment, step by step](#enrolment-step-by-step)
- [What is issued](#what-is-issued)
- [Publication: the root, the CRL and a colleague's certificate](#publication-the-root-the-crl-and-a-colleagues-certificate)
- [Trust on the clients](#trust-on-the-clients)
- [Revocation](#revocation)
- [Rotation](#rotation)
- [A renamed domain or address](#a-renamed-domain-or-address)
- [Backup and restore](#backup-and-restore)
- [The routes](#the-routes)
- [Who may](#who-may)
- [What it is not](#what-it-is-not)

Who holds what
--------------

**The person's private key never leaves the browser.** The authority signs; it
does not generate anybody's key. The webmail makes the key pair with the
browser's Web Crypto API, builds a PKCS#10 certificate request signed by that
key, wraps the private key under the account password exactly as an imported
key is wrapped, and sends only the request. The server signs it and answers the
certificate and the root; the page stores certificate, root and wrapped key in
the account's own S/MIME store, the same one an imported PKCS#12 goes into.

That is the zero-access design of this server's end-to-end encryption: it holds
nothing that decrypts a person's mail. An authority that generated the keys
could decrypt everybody's, and this one cannot. The consequence has to be said
as plainly as the benefit: **nothing on this server can recover mail encrypted
to a key that was lost.** A forgotten password that the keys were wrapped under,
a store deleted, a browser that never finished storing the key - each leaves the
mail encrypted to that key unreadable, by anybody, for ever.

**The authority's own key is on the server**, because it signs there. It is the
root's private key, PKCS#8, stored in `hm_smimeca.caprivatekey` protected by the
platform's secret store itself, whatever `ProtectStoredSecretsWithDPAPI` says:

- **On Windows, DPAPI with the machine's key** (`CRYPTPROTECT_LOCAL_MACHINE`).
  The envelope opens on this machine and nowhere else - and on this machine to
  any process that asks, the service and the administration tools alike, which
  is what machine scope means. Whoever can run code on the server can open it.
- **On Linux, the stored-secret key file** (`<DataFolder>/.hmailserver-secret-key`,
  AES-256-GCM, mode 0600). The envelope opens wherever that file is: carry the
  data directory with the database and the key goes with them; carry the
  database without it and the key opens nowhere.
- **Never the legacy scheme.** The server's older secrets fall back to a
  Blowfish form whose key is in the program when DPAPI fails, or when
  `ProtectStoredSecretsWithDPAPI` is off; an authority's key does not. When
  the store is unavailable the authority is not created (503, with the reason,
  and on Linux the error log names what is wrong with the key file).

The audit trail never records the key (the column's name is one it withholds).

Creating the authority
----------------------

In the **Control Deck**, open the domain and find **S/MIME authority**, after
DKIM key rotation. With none, it says so and offers a key choice and **Create an
S/MIME authority**. Over REST:

```
POST /api/v1/domains/{domain}/smime-ca     {"key": "p256"}      (or "rsa3072")
```

The root is self-signed: **CN "{domain} S/MIME Authority"** (the words alone
when a very long domain name would pass the 64 characters a commonName holds),
basicConstraints **CA:TRUE, pathlen:0** (it signs people, never another
authority), keyUsage **keyCertSign and cRLSign**, extendedKeyUsage
**emailProtection**, a critical **name constraint permitting the domain's own
addresses and nothing else**, a random 127-bit serial, a subject and authority
key identifier, **ten years**. **P-256** is the default: small, fast and
supported by every S/MIME client in use; **RSA-3072** is there for a client that
insists on an RSA chain.

The last two are what a root's key leaking would otherwise cost: installed as a
trusted root on every client, an unconstrained root vouches for any address in
the world and for TLS servers too; this one vouches for S/MIME at the domain's
own addresses, and clients that honour the constraints - OpenSSL, Windows,
Thunderbird's NSS - refuse anything else from it. A domain whose name is not
written in plain ASCII letters, digits, dots and hyphens gets no constraint. A
root keeps the extensions it was made with: an authority made by an earlier
build of this server is rotated to gain them.

The key is protected before anything is written, and when the platform's store
cannot protect it, nothing is created. A second create is 409 - the way to
replace a root is to rotate it.

The Deck then shows the root's subject, key type, fingerprint (SHA-256 of its
DER) and end, **Download the root** (the PEM), the rotation, and the
certificates issued. It shows a warning when the root's private key **does not
open on this machine**: a database or a backup carried here from a machine
where its envelope was made (above). Such an authority signs nothing - an
enrolment is refused with 503 and a sentence saying so, its CRL is not served,
and the error log says why once per root - and **must be rotated** on the new
machine.

The **Control Panel shows nothing of the authority itself**. Its domain dialog
reads a domain over COM (a domain's DKIM rotation, for one, is
`Domain.DKIMRotationStatus`), and the authority has no COM surface, so there is
nothing that dialog could read. It is managed in the Deck and over REST. The one
server-wide setting, `smime_authority_base_url` (below), is on the Control
Panel's Connections & protocols > Webmail page, the Encryption tab, which reaches
it over the REST API, so the REST API has to be on.

Enrolment, step by step
-----------------------

A person with an authority in their domain sees, under **Settings → Security →
S/MIME**, **From your organisation** with **Get a certificate from {domain}**.
A domain with none says so there, in one sentence, and offers nothing.

1. The person picks the key type. **RSA 2048** is the default and the one to
   choose: it signs and it is encrypted to. **P-256** is offered with the
   sentence that goes with it - the webmail has no elliptic-curve key agreement,
   so a P-256 certificate from this page **signs, and neither this webmail nor
   a colleague's can encrypt to it or decrypt with it**.
2. The page makes the key pair and a PKCS#10 request: subject CN (the address)
   and emailAddress, and an extensionRequest asking for the address as a
   subjectAltName rfc822Name. The request is signed by the new key.
3. The page asks for the password that will wrap the private key - the account
   password, which is what the store is opened with at sign-in - **before**
   anything is sent, so that a cancelled dialogue leaves nothing issued.
4. `POST /api/v1/me/smime/enroll {"csr": "<base64 DER>", "keep": false}`, from
   the account's own sign-in: an app password - a mailbox credential for a
   program - and an administrator's support session are refused with 403,
   since whoever holds the key reads the mail encrypted to it and signs as the
   person. The server checks, in this order: the domain has an authority (404
   with the sentence otherwise); the request parses; its key is **RSA of 2048
   to 8192 bits, or P-256 or P-384** - the floors the gateway encryption holds a
   partner's certificate to; it **verifies with its own key** (the proof that
   whoever asks holds it); it names **exactly one address** of at most 128
   characters (what a certificate's emailAddress holds), and that address is
   **the account's own, or an active alias of the account's own domain that
   points at it**. Not an address another account granted it the right to send
   as, not a proved external address, not a masked address: the authority
   vouches for who owns a mailbox, and those are somebody else's, or nobody's.
   An address that is not ASCII is refused, since a certificate's rfc822Name
   holds nothing else (RFC 8398's SmtpUTF8Mailbox is not written). Each refusal
   is a 400 naming what was wrong. Then the account's limits: **five
   enrolments a day** (429 past that - each one writes a row, and once it
   supersedes, a permanent entry on a public CRL) and **ten valid certificates
   at once** (409 until one is revoked).
5. The server signs, stores the certificate in `hm_smimeissued`, and answers it
   with the root as its chain. The certificate is **pending**: it is not
   answered to colleagues yet, and it has superseded nothing.
6. The page stores certificate, chain and wrapped key in the account's store,
   exactly as an import does, and the key is unlocked for the session. **That
   store is the confirmation**: the server sees the certificate arrive in the
   account's own store - the one place its key is used from - and only then is
   the certificate answered to colleagues and, unless kept, are the address's
   earlier certificates revoked as *superseded*. If the store refuses it (the
   store holds twenty certificates), the page revokes the new certificate at
   once as *cessationOfOperation*, since nobody holds its key, and **the earlier
   certificate stays valid**. A client that keeps its key somewhere else
   confirms with `POST /api/v1/me/smime/issued/{serial}/confirm`.

**At most one valid certificate per address.** A confirmed enrolment revokes the
ones before it for the same address (reason *superseded*), which is what a
person getting a new key wants; from then on the webmail signs with the newest,
and keeps the older ones' keys, which still open the mail sent to them. A
person with **two devices**, each keeping its own key, ticks **Keep my other
certificates for this address** (`"keep": true`) on the second, and both stay
valid; a colleague writing to them is given both and encrypts to both.

What is issued
--------------

| Field | Value |
|---|---|
| Version | X.509 v3 |
| Serial | The domain's next serial: 1, 2, 3... carried on to a new root by a rotation, so that a serial names one certificate of the domain |
| Validity | **One year** from the moment it is signed (less five minutes for clocks), and never past the root's own end |
| Subject | CN = **the account's name as this server holds it** (first and last name; the address when it has none) - not what the request said - and emailAddress = the address |
| subjectAltName | rfc822Name = the address, and nothing else whatever the request asked |
| basicConstraints | critical, CA:FALSE, whatever the request asked |
| keyUsage | critical; digitalSignature and keyEncipherment for an RSA key; digitalSignature and keyAgreement for an elliptic-curve key |
| extendedKeyUsage | emailProtection |
| Key identifiers | subjectKeyIdentifier (hash) and authorityKeyIdentifier (the root's) |
| CRL distribution point | `{base}/smime-ca/{fingerprint}.crl`, when `smime_authority_base_url` is set |
| Authority information access | caIssuers `{base}/smime-ca/{fingerprint}.cer`, when it is set |
| Signature | ECDSA with SHA-256 under a P-256 root; sha256WithRSAEncryption under an RSA root |

`{fingerprint}` is **the root's SHA-256 fingerprint**, 64 lowercase hex digits:
a name a restore and a domain's rename both leave as it is, where a database
row's number or the domain's name would change under a certificate that is out
in the world for a year.

With **`smime_authority_base_url` empty** - the default - the certificates
carry **neither** of the last two extensions: nothing here knows the address
the clients reach this server by, and a distribution point that leads nowhere is
worse than none. What that costs: Outlook and Thunderbird cannot fetch the CRL
from the certificate, so a client outside this webmail that checks revocation
reports it as unknown (and one set to insist on a check treats the certificate
as unverifiable), and a revocation reaches such a client only when it has been
given the CRL another way. This webmail does not use the CRL at all: its check
reads the authority's own record (below). A certificate keeps what it was
issued with; setting the URL afterwards changes the certificates issued
afterwards.

Publication: the root, the CRL and a colleague's certificate
------------------------------------------------------------

The root and the CRL are public, and they are served over **plain HTTP by the
web services listener** (`WebServicesHttpPort`), never by the REST listener: RFC
5280 recommends HTTP for a distribution point, and mail clients fetch one
anonymously. No credential is asked for and nothing secret is answered.

| Path | What it answers |
|---|---|
| `/smime-ca/{domain}.pem` | The domain's current root, PEM (`application/x-pem-file`) - the one to hand out |
| `/smime-ca/{domain}.cer` | The same, DER (`application/pkix-cert`) |
| `/smime-ca/{domain}.crl` | The current root's CRL, DER (`application/pkix-crl`) |
| `/smime-ca/{fingerprint}.pem`, `.cer`, `.crl` | One root by its fingerprint - what an issued certificate names - the current one, or a retired one while a certificate it signed has not expired |
| `/smime-ca/{domain}/{number}.pem`, `.cer`, `.crl` | One root by its row's number: the form the first build of this feature wrote into certificates, still answered for one that carries it |

A domain with no authority, a root that is no longer published, a root whose
key does not open on this machine (for its CRL; the error log says why, once per
root) and anything else is the same 404. The domain's name in a path is its
name as written, with anything but a letter, a digit, a dot or a hyphen as
`%XX`.

**`smime_authority_base_url`** (`PUT /api/v1/settings`, the Deck's Settings
page, the Control Panel's Webmail page on its Encryption tab,
`SmimeAuthorityBaseUrl` in `hm_settings`) is where the clients reach that
listener, such as `http://pki.example.com`. It is refused unless it is
absolute - `http://` (or `https://` behind a proxy), a host of letters, digits,
dots and hyphens (a name that is not ASCII in its punycode form) with a port if
it needs one, and an optional path of letters, digits, `- . _ ~ / ! $ & ' ( ) *
+ ; = : @` and `%XX` - with no space, comma, quote, query or fragment, in at
most 200 characters, since it is written into every certificate. It has to be
true: the listener has to be reachable at that address from wherever the
clients are.

**The CRL** is X.509 v2, signed by the root it speaks for, with every
certificate that root revoked, each with its date and its reason code
(keyCompromise 1, superseded 4, cessationOfOperation 5), a CRL number that only
rises, and the root's authority key identifier. **thisUpdate** is when it was
made and **nextUpdate a week** later. It is made again the next time it is
fetched after one of the root's certificates was revoked - a revocation that
lands while a list is being made is never left out of the next one - and
otherwise at most once a day; an HTTP cache in front of it may keep it five
minutes. A retired root signs its own CRL for as long as it is published, as
long as its key opens on this machine: after a move to another machine it
cannot, and its CRL is the 404 above until its certificates have expired - the
reason to rotate before moving where that can be helped, and to give the
clients the new root.

**A colleague's certificate** is what a person needs to encrypt TO somebody:

```
GET /api/v1/me/directory/{address}/smime
```

answers, to an authenticated account, the **confirmed** and valid (not revoked,
not expired) certificates the reader's **own domain's** authority issued for
that address, when it belongs to an **active account of the reader's own
domain** and **is still that account's** - its own address, or an alias still
switched on and still pointing at it - each with serial, fingerprint, validity,
base64 DER and PEM, and the root that signed them. An alias deleted, switched
off or pointed at somebody else takes its certificate out of reach at once,
since mail encrypted to it would be for a key the address's new owner does not
hold. It is 404 for an address of another domain, an inactive account, an
address the account no longer holds, a certificate pending, revoked or expired,
and while the directory is switched off (`directory_enabled`), as the
directory's photo route is; the same 404 each time. It is answered **whether or
not the person opted out of the directory's listing**: the opt-out takes a
person out of the list, not out of reach of somebody who already has the
address ([Directory.md](Directory.md), "The opt-out is not invisibility").

The webmail asks it by itself. A message to a recipient of the reader's domain,
with **Encrypt** ticked and no usable certificate of theirs in the store, is not
refused at once: the page looks the address up once, keeps **every**
certificate it is answered as a correspondent's certificate - the store the
certificates taken from signed mail go into - and encrypts to all of them. The
store marks such a certificate (`hm_smimekeys.smimeauthority`, `authority: true`
in `GET /api/v1/me/smime`): **the server sets the mark from the SHA-256 of the
certificate's own bytes**, which must also be the fingerprint the page gave, so
a page cannot claim it. And `GET /api/v1/me/smime` answers, for every marked
certificate, where the authority's record has it **now** (`authority_status`:
valid, revoked or expired): once a colleague's certificate is revoked or
superseded, the page stops encrypting to it at its next reading of the store,
and asks the directory again.

Trust on the clients
--------------------

A signature verifies in **Outlook, Thunderbird, Apple Mail or a phone** only
once the client trusts the root. **The root must be installed on the clients**:

- **Windows and Outlook**: by group policy (Computer Configuration → Policies →
  Windows Settings → Security Settings → Public Key Policies → Trusted Root
  Certification Authorities), or by hand into the user's Trusted Root store.
- **Macs, iPhones and Android**: by the device management the organisation
  uses, as a trusted root profile, or by hand.
- **Thunderbird**: Settings → Privacy & Security → Certificates → Manage
  Certificates → Authorities → Import, with "This certificate can identify
  email users" ticked.

The root to install is **Download the root** in the Deck, or
`/smime-ca/{domain}.pem` from the web services listener. Those clients learn of
a revocation only from the CRL, and so only with a base URL set (above).

**This webmail trusts the domain's authority by itself, and reads its record
rather than its CRL.** `POST /api/v1/me/smime/chain` - the check the page makes
on a signed message - trusts, beside the machine's own store, the reader's own
domain's **current root, and a retired root while a certificate it signed has
not expired**. A chain that ends at one of them is trusted only while its leaf
is **in the authority's record and not revoked**: a revoked certificate is
answered `trusted: false` with `revoked (<reason>)` from the moment it is
revoked, and a certificate the record does not have - which is what one made
with a root's key that left this server would be - with `not issued by this
authority`. The answer's `authority: true` says the chain ended at the
domain's authority, and the page's badge says **Signed by {name}, certified by
your organisation**. A chain ending at any other self-signed root is still
refused. So a colleague's signature shows as verified in the webmail on the
day the authority is created, whatever the machine's store says.

Revocation
----------

- **By an administrator**: the Deck's issued list, with a reason (key
  compromise, superseded, cessation of operation) and **Revoke**; or
  `DELETE /api/v1/domains/{domain}/smime-ca/issued/{serial}?reason=keyCompromise`.
- **By the person**: the list under **From your organisation** in the webmail,
  **Revoke**; or `DELETE /api/v1/me/smime/issued/{serial}` - the account's own
  certificates only, another's being the same 404 as none, and from the
  account's own sign-in, not an app password or a support session.
- **By a confirmed enrolment** for the same address, unless it keeps the old
  one: *superseded*.
- **By the store refusing a new certificate's key**: that certificate,
  *cessationOfOperation*.
- **By the account's deletion**: every valid certificate issued to it is revoked
  with *cessationOfOperation* and stays on the CRL, and the account's number is
  taken off every certificate issued to it, since account numbers are handed
  out again and a certificate must never become the next account's.
- **By the address's erasure** (`POST /api/v1/addresses/erase`,
  [PersonalData.md](PersonalData.md)): a valid certificate for the address is
  revoked with *cessationOfOperation*, and the address, the account and the
  certificate itself - whose subject is the person's name - are taken off its
  row; the serial, the fingerprint and the revocation stay, because the CRL
  must go on listing a certificate that is still out there.
- **By the domain's deletion**: the authority and everything it issued are
  deleted with it (the foreign keys). Nothing is left to revoke, and nothing is
  published.

What a revocation does: the certificate is no longer answered to colleagues;
this webmail's chain check refuses it at once; a colleague's page stops
encrypting to it at its next reading of the store; it shows as revoked in the
lists; and it is on its root's CRL the next time the CRL is fetched, which is
how every other client learns of it. A revocation cannot be undone, and a
certificate revoked twice keeps the date and the reason of the first - they
may be on a CRL already. The default reason, where none is given, is
*keyCompromise*.

Rotation
--------

**Rotate the authority** in the Deck, or
`POST /api/v1/domains/{domain}/smime-ca/rotate {"key": "p256"}`, makes a **new
root** that signs every enrolment from then on, and takes over the serial
counter. The old root is **retired, not deleted**: while a certificate it signed
has not expired it keeps its key, signs its own CRL, stays **published** and is
trusted by the webmail's chain check for those certificates (a revoked one
never). When the last of them has expired it has nothing left to vouch for: its
key is **wiped** - at the next create, rotation, enrolment or backup - the
backup stops carrying it, it is no longer published and the chain check no
longer trusts it. A root retired with no certificate alive loses its key at the
rotation itself.

What the clients must do: **install the new root beside the old one**, by the
same group policy, profile or hand. A client that has only the old root verifies
certificates issued before the rotation and not those issued after it. Nobody
has to enrol again because of a rotation; each person's certificate is renewed
by enrolling again before its year is out, and that one chains to the new root.

Rotate when the root's key does not open on this machine (a database or a
backup from elsewhere), when the domain has been renamed (below), before the
root's ten years run out - and **when the key may have been exposed**. In that
last case rotation is not enough on its own: the old root stays trusted for the
certificates it signed until the last of them expires, so **revoke every
certificate it signed** (each person enrols again under the new root) and
**take the old root off the clients**. A certificate somebody made with the
exposed key is refused by this webmail anyway - it is not in the authority's
record - but a client that trusts the old root would accept it, within the
root's name constraint, until the root is removed from it.

A renamed domain or address
---------------------------

The public URLs name a root by its fingerprint, so a domain's rename leaves
every issued certificate's CRL and root URL working. What a rename does change:

- **A domain renamed**: the root's CN and its name constraint still name the old
  domain, and a certificate for an address of the new name would verify
  nowhere, so the enrolment is refused (409) with the sentence to **rotate the
  authority**. Certificates issued before name the old addresses; the people
  enrol again after the rotation. `/smime-ca/{domain}.pem` answers under the
  new name.
- **An account or an alias renamed**: its certificates name the old address and
  are no longer answered to colleagues under it once the address is not the
  account's; the person enrols again for the new one, and may revoke the old.

Backup and restore
------------------

The authority is in the **configuration backup**, under its domain
(`Domain::XMLStore`): each root's certificate, its **key envelope as it is
stored** (none for a retired root that is no longer published), its created and
retired times and the serial counter, and every certificate it issued - pending
or confirmed - with the address of the account it was issued to, which the
restore finds the account's new number by. A restore deletes every domain first
and the authority with it, so the archive is where it comes back from. Every
value is checked as it comes back: a certificate that does not parse, a
fingerprint that is not one or an address that is not one is left out and said
in the error log, and a key that is not an envelope of a machine's secret store
is not restored as a key.

Where the key opens after a restore is where its envelope opens (see *Who
holds what*): on Windows, the machine that wrote it; on Linux, wherever the key
file is. A backup restored there brings back a working authority, and every
certificate issued before the restore keeps its CRL and root URLs, which name
the root by its fingerprint. Restored anywhere else it brings back an authority
whose key opens to nothing - the Deck says so, enrolment is refused and its CRL
is not served - which **must be rotated** there. The same is true of a database
carried to another machine.

**Guard the archive as you guard the server.** The envelope protects the key
from whoever holds the archive only as far as they cannot reach the machine (or,
on Linux, the key file); on the server itself it opens. Use the backup's own
encryption for archives that leave it.

The routes
----------

| Route | What it does |
|---|---|
| `GET /api/v1/domains/{domain}/smime-ca` | The authority (null when there is none), the retired roots, the newest 200 certificates issued, `base_url` and the paths of the root and the CRL |
| `POST /api/v1/domains/{domain}/smime-ca` | Create it (`key`: `p256` or `rsa3072`) |
| `POST /api/v1/domains/{domain}/smime-ca/rotate` | A new root (`key` as above) |
| `DELETE /api/v1/domains/{domain}/smime-ca/issued/{serial}?reason=` | Revoke one |
| `POST /api/v1/me/smime/enroll` | Sign the account's request (`csr`, `keep`); the certificate is pending |
| `POST /api/v1/me/smime/issued/{serial}/confirm` | Confirm one of the account's own whose key is kept outside its store |
| `DELETE /api/v1/me/smime/issued/{serial}?reason=` | Revoke one of the account's own |
| `GET /api/v1/me/directory/{address}/smime` | A colleague's confirmed, valid certificates |
| `GET /api/v1/me/smime` | Gains `authority`: whether the domain has one, its root, and what it issued to this account; and `authority_status` on each stored certificate the authority issued |
| `POST /api/v1/me/smime/chain` | Gains `authority`: whether the chain ended at the domain's authority; a revoked or unknown leaf is not trusted |

The OpenAPI document (`/api/v1/openapi.json`) describes each.

Who may
-------

Reading a domain's authority needs **view_configuration** for that domain, as
reading its DKIM rotation does; creating, rotating and revoking need
**manage_accounts** for that domain, as the rotation's actions do - the
domain's administrator, and a server administrator. A credential confined to
some domains reaches only theirs, and a read-only API key reads and changes
nothing. The `/api/v1/me` routes answer an account's own credentials and
nothing else, and the three that act - enrol, confirm, revoke - its own
sign-in, not an app password or an administrator's support session.

Every change is on the **audit trail** through the tables it writes
(`hm_smimeca` as "S/MIME authority", `hm_smimeissued` as "S/MIME certificate"),
each row named: a root created, rotated, retired or its key wiped under the
domain's name; a certificate issued, confirmed or revoked under its address and
serial, under whoever did it - an administrator, the account itself, or whoever
deleted the account; a deleted account's number taken off its certificates
under its address; an erased certificate under its serial alone. Deleting an
account that was issued nothing writes nothing here. The root's private key is
recorded only as "(changed)".

What it is not
--------------

- **Not Active Directory Certificate Services**, and not connected to one. It
  cannot enrol against an AD CS template or issue from an enterprise CA's key,
  and AD CS cannot issue from it.
- **No OCSP.** Revocation reaches other clients by the CRL, and only when a base
  URL is set (above); this webmail reads the authority's record instead.
- **No key escrow**, and therefore **no recovery of mail encrypted to a lost
  key**. The private keys are made and kept in the browsers, wrapped under the
  people's passwords; this server never has one, so it cannot give one back.
  If that is a requirement - a regulator's, or an organisation's - this is not
  the authority for it.
- **No elliptic-curve encryption in the webmail.** A P-256 certificate from it
  signs; nobody using this webmail can encrypt to it.
- **One address per certificate**, the account's own or an alias of its own
  domain; not a domain alias's form of the address, not another domain's alias.
- **Issued on enrolment, not pushed.** A person gets a certificate by asking
  for one in the webmail; nothing is issued to every account at once.
- **No COM, and of the Control Panel only the base URL.** The Deck and REST are
  the surfaces; the Control Panel's Webmail page sets `smime_authority_base_url`
  on its Encryption tab, over the REST API.
- **No automatic renewal.** A certificate lasts a year; the person enrols again
  before it runs out, and the webmail lists when each ends.
- **The CRL is made per server process and cached in memory**; two servers on
  one database each make their own, numbered from the clock.
