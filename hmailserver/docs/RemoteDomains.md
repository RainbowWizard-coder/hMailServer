Remote domains: TLS, limits, encryption and recipient verification
==================================================================

A remote domain policy says what this server will do when it talks to one named
domain it does not host: a partner, a customer, a bank, a provider. It holds four
kinds of decision, in one record because they are one subject:

* **Transport security** - whether mail *to* the domain must be encrypted, and how
  strongly; whether mail *from* the domain must arrive encrypted.
* **What this server will attempt** - the largest message it will send there, how
  many connections it holds open to it at once, how many messages a minute it sends
  it, and whether an automatic reply or a forward may go there at all.
* **Recipient verification** - whether an address at the domain is checked with the
  domain's own server before this server accepts mail for it. This is for a domain
  this server is a backup MX for, and for nothing else.
* **Gateway encryption** - whether a message to the domain is encrypted at this
  server to the partner's own OpenPGP key or certificate, whatever the sender's
  client did (below).

Policies live in the database (`hm_remotedomainpolicies`, schema 6045; the gateway
encryption columns since 6094), take effect for the next message without a restart,
and are part of the configuration backup.
Nothing about them is in `hMailServer.ini`.

They are edited four ways, which all write the same records:

| Where | How |
|---|---|
| Control Panel | **Mail flow & delivery > Remote domains**, beside Routes |
| Control Deck | **Remote domains** in the navigation |
| REST API | `GET`/`POST /api/v1/remote-domains`, `PUT`/`DELETE /api/v1/remote-domains/{id}`, `GET /api/v1/remote-domains/effective?domain=...`, `POST /api/v1/remote-domains/verification-cache/clear` |
| COM | `Application.Settings.RemoteDomainPolicies` |

All of them are server-administrator only. An API key restricted to named domains
is refused, because a policy names somebody else's domain and decides this server's
posture rather than one hosted domain's.


A policy is not a route
-----------------------

A **route** says *where* mail for a domain goes: a target host and port, a
credential, and it replaces the MX lookup. A **policy** says what this server will
and will not do when it gets there, however the server was found. A domain may have
both, and the policy applies to a routed delivery exactly as it applies to an MX
one - a policy that a route quietly exempted from its TLS requirement would be the
silent downgrade this feature exists to prevent.


Which policy applies
--------------------

A policy names a domain (`bank.example`) or a wildcard pattern (`*.example`, `*`).
For each delivery the **most specific active** policy governs:

1. an exact name beats any pattern, however long the pattern;
2. between two patterns, the one with more literal characters wins;
3. an inactive policy is skipped, and the next most specific active one governs in
   its place. Switching off the entry for one domain beneath a `*` that requires TLS
   removes an exception; it does not exempt that domain from the rule every other
   domain still obeys.

`GET /api/v1/remote-domains/effective?domain=mail.bank.example` (and
`PolicyForDomain` over COM) answers which record a delivery will use, so this can be
checked before a message finds out.

When one connection carries recipients of several domains - several domains routed
to the same smart host, for instance - the strictest combination applies: the
highest TLS requirement, and the smallest of each non-zero limit.


Outbound TLS
------------

| Value | What delivery requires |
|---|---|
| `none` | Nothing beyond what the server would do anyway: the SMTP connection security setting, the route's own security, and any MTA-STS or DANE policy the domain publishes. |
| `encrypted` | STARTTLS must be offered and the handshake must succeed. The certificate is not judged - the level for a partner with a self-signed or internal certificate. |
| `verified` | Encrypted, and the certificate must chain to a trusted root and name the host being connected to - what MTA-STS `enforce` requires. |
| `dane` | Encrypted, and the certificate must match a DNSSEC-validated TLSA record published for the host (RFC 7672). A host with no usable validated record is skipped. The TLSA lookup is made for such a policy even when `DaneEnabled` is off and even for a routed delivery; with `DnssecValidationEnabled` off no record counts as validated, so every delivery under the policy is deferred. |

**How this composes with MTA-STS and DANE.** Those two already require TLS for a
domain that publishes a policy. A remote domain policy is the administrator saying
so for a domain that does not. The three combine as the *strongest* of them, never
the newest: MTA-STS `enforce` and a usable TLSA record each raise the requirement on
their own, and a policy here can only raise it further. A domain that publishes an
MTA-STS enforce policy and is named here at `encrypted` still gets certificate
verification, because MTA-STS asked for it. There is no value on this page that
weakens what a domain published for itself.

**What a refusal looks like.** A delivery a policy cannot satisfy is **deferred**,
never sent in the clear and never silently downgraded. The recipient's status is
`4.7.0`, the message stays in the queue and is retried on the usual schedule, and
the text an administrator reads - in the delivery log and, if the retries run out,
in the bounce - names the rule:

    Delivery deferred: the remote domain policy for bank.example requires TLS with a
    certificate that verifies to this destination, and it could not be established.
    The message has NOT been sent in the clear.

It is a deferral rather than a bounce on purpose. The remote is a third party, and
its STARTTLS - or its certificate - may be fixed before the queue gives up; RFC
8461 section 5 reasons the same way about MTA-STS. A DANE requirement no host can
meet is deferred with `4.7.0` and a sentence naming the policy in the same way.

**Inbound TLS.** With **Require TLS on mail from this domain** set, a sender whose
envelope address is at the domain is answered `530 5.7.0 Must issue STARTTLS first`
at `MAIL FROM` on an unencrypted session. It is not applied to an authenticated
submission (one of this server's own users), and not to a connection from a
configured **incoming relay**: behind a front-end filter or gateway, the hop the
policy is about happened before that machine, and inbound TLS has to be required
there. Mind that the envelope sender is whatever the connecting server claims; this
is a transport rule for honest partners, not an anti-spoofing control.


Limits
------

| Setting | 0 means | Beyond it |
|---|---|---|
| Largest message (KB) | no limit of ours | The message is **not attempted**: no connection is opened, and it is bounced at once with `5.3.4`, naming the policy and both sizes. This is the one refusal that is permanent - a message will not be smaller on the next attempt. |
| Simultaneous connections | unlimited | The delivery is deferred with `4.4.5` for one to five minutes. Delivery threads never wait for a slot, because a thread waiting is a thread not delivering anybody else's mail. The count covers every domain the policy's pattern matches. |
| Messages a minute | unlimited | Deferred with `4.4.5` for one to five minutes, as the server-wide `MaxOutboundPerDestinationPerMinute` does; the two are independent and both apply. |

Both are deferrals this server chose, and since 23 September 2026 neither
counts as an attempt: the message's next try is one to five minutes on and its
tries counter is untouched, so a tight ceiling delays mail by minutes and
bounces it only once it has been queued for longer than its retry schedule
spans (before that, a deferral waited for the route's retry interval and
spent one of its retries). The same rule, the server-wide ceiling for domains
no policy names, the tenant share, and the back-off a destination's own
slow-down reply opens are in [OutboundShaping.md](OutboundShaping.md); a policy's
connection ceiling wins over the server-wide one wherever the policy sets it.

**Automatic replies and forwarding.** With **Automatic replies may go to this
domain** cleared, no out-of-office reply - an account's own or the domain-wide one -
is sent to an address at the domain. With **Mail may be forwarded to this domain**
cleared, an account's forward to an address there is not made (the message is still
delivered to the mailbox), and neither is a Sieve `redirect`. Both are logged at
debug level.


Gateway encryption
------------------

With **Encrypt mail to this domain at this server** set (`encrypt` over REST,
`Encrypt` over COM), a message to the domain is encrypted *here*, to the partner's
own key, whatever the sender's mail client did - the enterprise shape of end-to-end
mail, and what an organisation's encryption gateway (Zix, Cisco, Proofpoint,
SEPPmail) does between organisations. The partner's gateway decrypts it on arrival
and hands its people the message that was written. TLS protects the connection,
which ends at the partner's first server; this protects the message, wherever it is
kept or relayed after that.

| Method | What the partner receives |
|---|---|
| `openpgp` | PGP/MIME (RFC 3156): `multipart/encrypted; protocol="application/pgp-encrypted"`, the version part and the armoured message, encrypted to the encryption subkey of the partner's OpenPGP public key - RSA of 2048 bits or more, or Curve25519 (cv25519) - whose binding signature verifies against the primary key, by the writer the zero-access mailbox uses ([ZeroAccess.md](ZeroAccess.md)). |
| `smime` | S/MIME (RFC 8551): `application/pkcs7-mime; smime-type=enveloped-data; name="smime.p7m"` with `Content-Disposition: attachment; filename="smime.p7m"`, AES-256-CBC for the content, and the content key sent to the certificate by RSAES-OAEP (RFC 3560, with its default SHA-1 parameters, which every S/MIME client reads) for an RSA certificate of 2048 bits or more, or agreed with it by ECDH and the X9.63 KDF over SHA-256 (RFC 5753) for an elliptic-curve certificate of 256 bits or more. |

**The key.** One key per partner domain - the partner's gateway key, which is how
gateways publish theirs - and **not** one per recipient address: an armoured OpenPGP
public key, or one or more PEM certificates, of which the first is the one encrypted
to and any after it are kept as its chain. White space around it is not part of it,
and white space alone is no key: it removes the key held, on every surface.

It is judged when the policy is saved, in two steps.

* **Whatever the method** - encrypting or not - a paste that carries private-key
  material *anywhere* in it is refused with a sentence, and nothing is stored: a PEM
  `PRIVATE KEY` block of any kind (PKCS#8, `ENCRYPTED PRIVATE KEY`, `RSA PRIVATE KEY`,
  `EC PRIVATE KEY`, OpenSSH), or an OpenPGP `PRIVATE KEY BLOCK` or `SECRET KEY BLOCK`,
  on its own or beside a public key or a certificate. So is a paste over 60,000
  characters, which is a keyring or a certificate bundle rather than a gateway key.
* **When the save changes the key or the method** - a new policy, a key that differs
  from the one held, encryption switched on or to the other method - the key must be
  one a message can be encrypted to, and is refused with a sentence when it cannot be
  read, has expired, or cannot be encrypted to: an OpenPGP key with no encryption
  subkey whose binding verifies (below), or whose encryption key is RSA under 2048
  bits; a certificate that is not valid yet, whose key usage has no
  `keyEncipherment` (`keyAgreement` for an elliptic-curve one), whose key is neither
  RSA nor elliptic-curve, or whose key is RSA under 2048 bits or a curve under 256.
  (An OpenPGP key's creation time is not compared with the clock: "not valid yet" is
  a certificate's notBefore only.) A key given while the policy encrypts nothing is
  judged as what it looks like - PEM certificates as S/MIME, anything else as
  OpenPGP - so that a key kept for later is one that will work; switching encryption
  *off* judges nothing.

A save that changes neither the key nor the method does not judge the key again. So
once a partner's key has expired, the policy's other fields - the fallback above
all, which exists for exactly that day - can still be saved, and the listing's
`encrypt_key_problem` says what is wrong with the key held. A key that names no
address or host in the partner's domain is **accepted with a warning**, because a
gateway's key often names the gateway rather than the domain: read the fingerprints
out to the partner before relying on it.

**Which fingerprint the partner confirms.** For S/MIME, the SHA-256 of the
certificate. For OpenPGP, two: the primary key's (`encrypt_key_fingerprint`), which
is the number a partner publishes, *and* the subkey's that messages are encrypted to
(`encrypt_key_subkey_fingerprint`); `gpg --fingerprint --fingerprint <key>` prints
both, and the partner confirms both. A subkey is the part of an OpenPGP key a
stranger can append without changing the primary's fingerprint - a keyserver that
merges packets unchecked, a key altered in transit - so this server encrypts only to
a subkey whose binding signature verifies against the primary key, and takes a
subkey's revocation only when that verifies too (RFC 9580 section 5.2.4; RSA,
Ed25519 and ECDSA on the NIST curves are checked, and a SHA-1 signature only when it
was made before 2019, as GnuPG takes it). A key whose primary is of a kind this
cannot check - DSA, a Brainpool curve - has no subkey this server can show is the
partner's, and is refused. GnuPG discards a subkey whose binding does not verify the
same way, so the key an administrator checked in `gpg --list-keys` is the key
encrypted to.

The key is written and never read back. Over REST `encrypt_key` is write-only, and
over COM `EncryptKey` answers empty; what is held is described instead -
`encrypt_key_fingerprint`, `encrypt_key_subkey_fingerprint`, `encrypt_key_expires`,
`encrypt_key_subject`, `encrypt_key_problem` (why no message can be encrypted to it
now) and `encrypt_key_warning` in the listing; `EncryptKeyFingerprint`,
`EncryptKeyExpires` and `EncryptKeySummary` (which names the subkey too) over COM,
the last of which the Control Panel's dialog shows as **The key held**, and the
Deck's list shows the method with a **cannot encrypt** mark and the reason when there
is one. A save that leaves the key out keeps the one held, which is why the Deck's
box and the Control Panel's open empty and are never filled; over REST an empty or
white-space `encrypt_key` removes it. A REST `PUT` that leaves `encrypt` out keeps
the method too - the one other field it does not reset - so that a client written
before gateway encryption existed cannot switch a partner's encryption off by not
naming it. The key is public, so it is stored in the clear
(`hm_remotedomainpolicies.policyencryptkey`, schema 6094) and the configuration
backup carries it; the audit trail records that the column changed and never its
text (`policyencryptkey=(changed)`).

**A refused save.** Over REST a policy is changed on a copy, saved, and reaches
delivery by the collection being read again, so a refused body changes nothing and a
delivery in progress keeps the record it started with. Over COM - the Control
Panel's path - it is the same: the first setter called on a policy handed out of
the collection copies it, the object writes and reads its copy from then on, and
`Save` saves the copy and reads the policies again. So a change is not in effect
until `Save`, a refused key never governs a delivery, and after a refused save the
object is read back as the database holds it - what it reads is the policy in
effect.

**What is encrypted, and what is not.** The *whole* original message goes under the
key - every header and the body, exactly as it would have been sent, this server's
own DKIM signature included - so the partner opens the message that was written. The
copy's own header is the original's, less the fields that described or signed the
old body (every `Content-*` field, `MIME-Version`, `DKIM-Signature` and the three
`ARC-*` fields), with `X-hMailServer-Gateway-Encrypted` naming the method and the
key - `OpenPGP to key <primary> subkey <subkey>`, or `S/MIME to key <fingerprint>`.
**The Subject is not hidden**, and neither are From, To, Cc, Date, Message-ID or the
Received trace: they are headers, and a gateway that has to route and file the
message reads them. Put nothing in a subject line that the partner's relays should
not see.

**Only the partner's copy.** A message to the partner and to anybody else - a local
account, another domain - is encrypted for the partner's recipients only; everybody
else gets it as it was, from the queued message, which is never rewritten. When
several domains share one smart host, the recipients whose policy encrypts travel in
a transaction of their own, one partner key to a transaction.

**Already encrypted, and signed.** A message that is encrypted end to end already
goes as it is, and the application log says so: a second layer would hand the person
the first one unchanged. What decides it is what the body holds, never the label:
an `application/pkcs7-mime` body whose CMS content type is enveloped-data or
authenticated-enveloped-data - whatever its `smime-type` says, and whether or not it
has one - or a PGP/MIME message whose encrypted part is an OpenPGP message that
begins with session key packets and then encrypted data. A message only *labelled*
encrypted is encrypted like any other: opaque signed-data labelled enveloped-data,
the bare `application/pkcs7-mime` S/MIME v2 clients used for opaque *signed* data, a
`multipart/encrypted` whose PGP MESSAGE is readable. (The first 12 KB of an OpenPGP
message are read, which hold the session key packets of any ordinary number of
recipients; one encrypted to more is taken for not encrypted and gets a second
layer, which is the safe way round.) A message that is *signed* but whose content is
in the clear (S/MIME `multipart/signed` or opaque `signed-data`, a PGP/MIME
signature) is encrypted, signature and all: the signature is inside, and still
verifies once the partner has decrypted it. [SealedMail.md](SealedMail.md) is the
rest of what the server does with a message that is sealed end to end.

**DKIM.** The copy is signed afresh, over its new body, by the same signer and key
the original was (the sending domain's DKIM settings; nothing is signed that would
not have been), and ARC-sealed afresh where ARC sealing is on. The original's
signature is inside, where it still verifies. A message this server relays for
somebody else - a forward of mail from outside - loses its author's DKIM signature
on the outside for the same reason, and keeps it inside. DKIM signs a file of at
most 50 MB, and the armour or the base64 makes the copy about 1.37 times the
original, so the copy of an original over about 36.5 MB goes **without** a DKIM
signature of this server's; the application log says so for that message.

**When a message cannot be encrypted** - the key has expired since it was saved, or
the message is over 50 MB (it is encrypted in memory, as DKIM signs it) - the
policy's fallback decides (`encrypt_fallback`, `EncryptFallback`, **When a message
cannot be encrypted**):

| Fallback | What happens |
|---|---|
| `refuse` (0, the default) | Nothing is sent. The recipients at the domain fail **permanently** with `5.7.10` ("encryption needed"), and the sender gets a delivery status notification naming the policy and the reason, saying the message has NOT been sent unencrypted. Permanent, as the size ceiling is: a retry would find the same key, the fix is the partner's new key, and the sender learns now rather than in two days. |
| `send_unencrypted` (1) | The message is sent as it was written, and the application log says so, naming the policy and the reason. |

The refusal is the default because an administrator who wrote "encrypt to this
partner" meant that nothing goes there in the clear; a default that sent it anyway
would be the silent downgrade the TLS requirement exists to prevent.

A failure of this server's *own* is neither: the message file locked by a virus
scanner, the temporary directory full, no memory, the cryptography library failing.
The next attempt may well succeed, so the batch is **deferred** with `4.3.0` and
retried on the usual schedule, whatever the fallback says, and the log says it was
not sent unencrypted.

**What it leaves behind.** One application log line per encrypted batch, written as
the copy is about to be offered - `Encrypted with OpenPGP to key <primary> subkey
<subkey> for 1 recipient(s), as the remote domain policy for partner.example
requires.` - and, where the message trace is on, an `encrypted` event for each
recipient naming the method, the key and the policy (`unencrypted`, with the reason,
for a message the fallback sent in the clear). The copy is made after every decision
that could hold the batch back without connecting - the back-off window, the rate
and connection ceilings, the tenant share, the MX lookup and MTA-STS
([OutboundShaping.md](OutboundShaping.md), *The order of the checks*) - so a batch
that is only going to be deferred is never encrypted, and never logged or traced as
encrypted. The size ceiling above is measured twice: on the queued message before
anything else, and on the encrypted copy, which is what is sent. The copy lives in
the server's temporary directory for the length of the batch and is removed on
every way out.

**What it holds in memory.** The message is read whole, up to the 50 MB ceiling.
Each form of it is let go of as soon as the next is made, and the copy is written to
its file in parts rather than joined, so a message of N bytes peaks at about three
times N while it is encrypted - the original, the plaintext packet the OpenPGP
writer builds and its ciphertext; for S/MIME, the original and what OpenSSL's CMS
holds of it. A few partner deliveries of 50 MB at once still take a few hundred
megabytes between them; the ceiling is what bounds it.

**What the partner has to do.** Hand over, or publish, the public key or certificate
of its gateway, and decrypt at that gateway - or give the private half to the people
it is for. Nothing else changes for it: the message arrives by SMTP like any other.

**What this server does not do.**

* **No signing at the gateway.** A signature made here would claim for the person
  what this server cannot know about them. People sign in their own client, and the
  signature travels inside.
* **No per-recipient keys, and no key discovery** - no WKD, LDAP or certificate
  directory lookup. The administrator pastes the partner's key, and the save judges
  it.
* **No decryption of replies.** This server holds no gateway private key and
  publishes none, so the partner's gateway has nothing of this server's to encrypt a
  reply to. A reply the partner encrypts is encrypted to the person's own key and is
  opened by their own client or not at all - which is what the person's side, the
  webmail's OpenPGP and S/MIME ([OpenPGP.md](OpenPGP.md)) and a
  [zero-access mailbox](ZeroAccess.md), is for.


Recipient verification (backup MX)
----------------------------------

A backup MX accepts mail for every address at the domain it backs up, because the
list of addresses is on the primary. While the primary is down, a message for an
address that does not exist is accepted, queued, refused by the primary when it
returns, and bounced - to the envelope sender, who in a dictionary attack is
somebody else. The backup has become a backscatter source. With **Verify a
recipient with the domain's own server** on, this server asks the primary at `RCPT
TO` time and refuses the address there, to the machine that is actually sending.

**How it asks.** An SMTP session: `EHLO`, `MAIL FROM:<>`, `RCPT TO:<address>`,
`QUIT`. No message is sent. The null sender is what RFC 5321 reserves for
non-delivery traffic, and it cannot be bounced to. The server asked is **The server
to ask**, or when that is empty the domain's MX hosts in preference order, skipping
any address and port this server itself listens on - which is the backup's own MX
record. At most two hosts are tried.

**What it decides.**

| The primary answers | This server |
|---|---|
| `2xx` to `RCPT TO` | accepts the recipient |
| `5xx` to `RCPT TO` | refuses it: `550 5.1.1 Recipient address rejected` |
| anything else - `4xx`, a refused null sender, no greeting, a timeout, no host to ask | **accepts**, exactly as it would have without the policy |

A verification that cannot be made never becomes a refusal. The primary's own
wording is written to this server's log and is not repeated to the sender.

**When it asks.** Only for a session that has not authenticated, only after the
relay and authentication decisions, the DNS blacklists and greylisting have had
their say, and never for a domain this server hosts. An address this server already
knows the answer to never reaches a verification.

**Safety settings.**

| Setting | Default | Why |
|---|---|---|
| How long to wait | 10 s, at most 60 | The wait is inside the sending server's SMTP session. It is one deadline over the whole conversation, the same on Windows and Linux. |
| How long an answer is remembered | 60 min | A flood of messages to one address costs the primary one session. A verification that gave no verdict is remembered for one minute at most, so a primary that comes back is asked again promptly. |
| How often one domain is asked, per minute | 10 | A ceiling on sessions to the primary. Beyond it recipients are accepted without asking - our own throttle never refuses somebody else's mail. |

**Two warnings.**

* *A callout can be used to enumerate a third party's addresses*, and to make this
  server a nuisance to someone else's. Turn it on only for a domain this server is a
  backup MX for, by agreement with whoever runs the primary. The rate limit and the
  cache bound what an abuser gets; they do not make the feature appropriate for a
  domain you do not serve.
* *Some primaries refuse the null sender* or answer every `RCPT TO` with `250` until
  `DATA`. Against those, verification gives no verdict and changes nothing - check
  the log for `gave no verdict` after turning it on.

When the primary has been fixed after refusing addresses it should have accepted,
**forget the remembered verdicts**: the button on the Deck view,
`POST /api/v1/remote-domains/verification-cache/clear`, or
`RemoteDomainPolicies.ClearVerificationCache()` over COM.


What is not here
----------------

* **Per-domain message format** (Exchange's "use rich text" and character-set
  choices). This server sends a message as it was written.
* **Recipient verification for a domain whose primary is this server's own
  database** - that is not a callout, and `RecipientParser` already answers it.
* **A per-policy SMTP port or credential** - that is a route's job.
* **Gateway encryption to one address, or keys looked up rather than pasted** - see
  *What this server does not do* under Gateway encryption.
