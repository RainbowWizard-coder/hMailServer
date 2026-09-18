Passkeys and single sign-on
===========================

Two ways in besides a password, for the same two pages a password opens: the
webmail at `/portal` and the Control Deck at `/`.

* A **passkey** is a WebAuthn credential held by the person's own device - a
  phone, a laptop's secure enclave, a security key. The private half never
  leaves it; what this server keeps is the public half. Signing in is the
  device's fingerprint reader, face or PIN, and nothing is typed and nothing is
  sent that could be captured and replayed.
* **Single sign-on** points the sign-in at the organisation's OpenID Connect
  issuer. The person signs in there, the issuer says who they are in an
  `id_token` this server verifies, and the verified address names a mailbox
  here.

Both are additions. A stock installation is unchanged: passkeys are offered but
nobody has one until they enrol it, and single sign-on does nothing at all
until an administrator names an issuer. The password, and the one-time code
where it is enrolled, stay exactly as they were and are the fallback for
everything below.

Passkeys
--------

### Enrolling one

The webmail's **Security** page has a Passkeys card, and the Control Deck has a
Passkeys view for the administrator's own. Both ask for the owner's password
again - the account's own password for an account, the administrator password
for the administrator, with the one-time code in `X-hMailServer-OTP` where a
second factor is enrolled, or for an account one of its recovery codes in
`X-hMailServer-Recovery-Code` in the code's place ([AccountRecovery.md](AccountRecovery.md));
the recovery code is spent by the registration it proved and by nothing less.
That is the same rule application passwords follow and it is there for the same
reason: a session somebody else is holding must not be able to add a way in that
outlives it. A request that was itself authenticated with an application
password is refused outright.

Over REST:

    POST   /api/v1/passkeys/options    what the browser needs to make one
    POST   /api/v1/passkeys            the credential it made
    GET    /api/v1/passkeys            the ones this credential holds
    DELETE /api/v1/passkeys/{id}       remove one

The four routes answer *whoever signed in*: an account's session gets that
account's passkeys and the administrator's session gets the administrator's.
There is no route by which one person administers another's, and an API key is
refused all four.

At most ten per owner. Removing one does not tell the device: it keeps a private
key this server no longer knows about, which is harmless because a credential
the server does not hold cannot sign anybody in.

Listing and removing are answered while `passkeys_enabled` is **off** as well.
Only the two routes that would add one are behind the switch, because the moment
an administrator most often turns passkeys off is the moment a device has been
lost - which is exactly the moment its owner wants that credential gone.

### Signing in

    POST /api/v1/passkeys/challenge    no credential; the challenge, and a cookie
    POST /api/v1/session               the assertion, in place of a password

The challenge is thirty-two random bytes kept in memory for five minutes,
against a nonce the browser carries in the short-lived `hmpasskey` cookie, and
it is **spent on first use** - so an assertion cannot be replayed. The sign-in
itself goes through `POST /api/v1/session`, the one place this server mints a
browser session, so a passkey sign-in gets the same session table, the same idle
and lifetime ceilings, the same one-per-device rule and the same device-list
entry that a password sign-in gets. The entry says `passkey`.

`allowCredentials` is deliberately empty: the browser offers whatever
discoverable credential it holds for this server, so a person does not have to
type an address first.

**A passkey stands in for the one-time code.** The registration insists the
authenticator reported *user verified* - it asked for a PIN, a fingerprint or a
face before it signed - so the credential is already two factors: the device,
and the person on it. TOTP is not asked for on top, and TOTP remains the
fallback for a password sign-in.

Every **assertion** insists on it too, and not only the registration. The
registration options and both pages' sign-in options ask the browser for
`userVerification: "required"` - asking for what is demanded, so the device asks
for the PIN instead of the person being refused afterwards - and the server checks
the flag whatever the browser was asked, so a credential enrolled as two factors
cannot later sign in as one - a stolen security key driven by a client that asks
for no verification is refused, where the account's password sign-in would have
demanded a code. The cost is that an authenticator with no way to ask for a PIN
or a fingerprint cannot be used here at all - but that was already so, because
the registration refuses one for the same reason.

### What is verified

At registration, from the attestation object:

* the client data's `type` is `webauthn.create`, its `challenge` is the one this
  server issued to this session, and its `origin` is the origin this page was
  served on (the `Host` header, with `https` required unless the host is a
  loopback name, which is what a browser treats as a secure context);
* the authenticator data's relying-party hash is SHA-256 of the relying party
  id, the *user present* and *user verified* flags are both set, and attested
  credential data is there;
* the COSE public key is ES256 (-7) or RS256 (-257) and nothing else, with the
  key type agreeing with the algorithm.

At sign-in:

* the same three things about the client data, with `type` `webauthn.get`;
* the relying-party hash, and the *user present* **and** *user verified* flags,
  both, exactly as the registration demanded both;
* the signature over `authenticatorData || SHA-256(clientDataJSON)`, with the
  stored key, in OpenSSL (ECDSA P-256 with SHA-256, or RSA PKCS#1 v1.5 with
  SHA-256);
* the signature counter has moved past what this server last saw. An
  authenticator that keeps a counter moves it on with every assertion, so one
  that has not is either a clone of the credential or a replay; it is refused,
  and the log says so. An authenticator that reports 0 every time keeps no
  counter, which is allowed and is not evidence of anything;
* the user handle, when the credential carries one, names the owner the
  credential id already named;
* and then the account itself, which is a separate question from the
  credential: it exists and is active, the compromised-account detection is not
  **holding** the name, and the **domain's country policy** allows a sign-in
  from where this one came. A single sign-on ticket meets the same two, for the
  same reason - they are what every credential that proves identity without a
  password meets in this tree (a client certificate, a Kerberos ticket, a bearer
  token on IMAP, POP3 and SMTP), and a credential that skipped them would be a
  way past a lock every other way in obeys. The last-logon stamp moves on here
  too, so somebody who only ever signs in with a passkey does not look to an
  administrator like somebody who has never signed in.

The per-name lockout that counts **failed passwords** is deliberately *not*
applied to either. It exists to slow guessing, and neither an assertion signed
by a key this server registered nor a ticket it minted two minutes ago is a
guess; applying it would let a run of bad passwords from anywhere lock a person
out of the credential that cannot be guessed. That is the same decision
`AccountLogon.h` records for the other passwordless credentials.

`POST /api/v1/session` demands `X-Requested-With: hMailServer` when the
credential is in the **body** - an assertion or a ticket rather than a password
in an `Authorization` header. A body is the one credential a cross-site form can
carry (`text/plain` needs no preflight, and no header of ours has to be set), so
without it another site could post an attacker's own ticket and quietly replace
the reader's session cookie with a session in the attacker's mailbox. The header
is the same proof of a same-origin script that every session-authenticated write
already gives, and both of this server's pages send it.

### What is NOT verified, and what that costs

**The attestation statement.** A registration carries an `attStmt`: a statement,
signed by a key the manufacturer certifies, saying what kind of device made this
credential. Verifying it needs the manufacturer root certificates (the FIDO
Metadata Service) and a policy about which models are acceptable. This project
ships neither, so the attestation object is read for its authenticator data
alone and the statement is ignored - and the registration asks for attestation
`none`, because asking for a statement nobody checks would be theatre.

What that costs: an installation **cannot** say "only hardware keys of this
make", and cannot tell a key in a certified device from one in a software
authenticator. What it still has is the property that matters for signing in:
this public key was registered by somebody holding the account's password, and
every assertion afterwards is checked against it.

### The relying party id

A passkey is bound to a *relying party id* - a domain name - and an
authenticator will sign for that id and no other. By default this server uses
the host the page was served on, read from the `Host` header. For a server
reached at one name that is right and needs no setting.

`passkey_relying_party` overrides it, and is what a server reached at several
names needs: set it to a registrable parent domain (`example.com`) and a passkey
made at `mail.example.com` works at `webmail.example.com` too. A value that is
not the page's host or a parent of it is refused by the browser, not by this
server.

Behind a TLS-terminating proxy, the `Host` header is still what the browser
sent, so the check holds; `X-Forwarded-Proto` is **not** consulted, which is why
the origin check accepts `https` at the page's host and plain `http` only on a
loopback name.

### The threat model

What a passkey is good against: a password guessed, reused, phished or read out
of a breach corpus. The credential is bound to this server's relying party id,
so a page at another origin cannot get an assertion the browser would make for
this one - which is the whole of the anti-phishing property, and it is the
browser's to enforce, checked here by the origin and relying-party comparisons
above.

What it is not good against: somebody who has the device *and* can unlock it;
an administrator of this server, who can remove a passkey and set a password; a
compromised browser, which can ask for an assertion the person then approves.
Nor does it protect a mailbox that still has a weak password, because the
password still works - passkeys are a way in beside it, not instead of it. An
installation that wants the password gone needs account-level policy this server
does not have; that is a gap, and it is named here rather than left to be found.

What it does **not** put a person outside: the account's own state. A passkey
does not carry its owner past the compromised-account hold, past a domain's
country policy or past an account that has been made inactive - see the sign-in
list above. That matters most in the case it was written for: a takeover that
had the password long enough to enrol a passkey of its own, and is then locked
out of the password, the app passwords, IMAP, POP3 and SMTP by the detection.
The passkey it enrolled is refused with them.

The challenge a sign-in answers is handed out to anybody, because a person
signing in with a passkey has no credential to present yet. The table holding
those challenges **evicts its oldest entry** when it is full rather than refusing
the newest, and single sign-on's table of unfinished sign-ins does the same: a
table that refused would be a switch a stranger could throw with a few hundred
requests, turning both buttons off for everybody for the next few minutes.

Single sign-on (OpenID Connect)
-------------------------------

### Setting it up

Register this server at the issuer as an **authorization-code client with PKCE**,
with the redirect URI `<oidc_redirect_base_url>/portal/oidc/callback`, then:

| Setting | What it is |
|---|---|
| `oidc_issuer` | `https://login.example.com` - the discovery document is read from `<issuer>/.well-known/openid-configuration` and cached for an hour. Empty offers no single sign-on at all. |
| `oidc_client_id` | The client id this server is registered under. |
| `oidc_client_secret` | Its secret, stored protected (the secret store, as the relayer password and the import client secrets are) and never emitted. Empty for a public client, which PKCE makes safe. |
| `oidc_scopes` | `openid email` by default, which is what the mapping needs. |
| `oidc_redirect_base_url` | The address the webmail is reached at from outside. Empty falls back to `import_oauth_redirect_base_url`. |
| `oidc_auto_provision` | Whether a verified address with no mailbox here gets one. Off. |
| `oidc_administrators` | The addresses that open the Control Deck as the administrator, comma separated. Empty means single sign-on reaches the webmail only. |

The two sign-in pages then draw a **Sign in with &lt;issuer host&gt;** button;
what they draw is decided by `GET /api/v1/signin-methods`, which carries two
flags and the issuer's host and says nothing about who exists here.

### The flow

    GET /portal/oidc/start[?deck=1]   to the issuer, with state, nonce and PKCE
    GET /portal/oidc/callback         back from it

The callback exchanges the code at the token endpoint and verifies the
`id_token`:

* the **signature**, against the issuer's published JWK Set by the `kid` the
  token names. RS256 and ES256 only - `none` would make the token anybody's to
  write, and HS256 would make it anybody's who holds the client secret;
* `iss` exactly as the discovery document declares it, and the discovery
  document itself must declare the issuer this server was pointed at;
* `aud` contains this server's client id;
* `exp` in the future and `iat`/`nbf` not in the future, with a minute of skew;
* `nonce` is the one this server put in the authorize request, held against the
  state.

Then `email` - and only when `email_verified` says the issuer verified it, since
an unverified email claim is a string somebody typed at the provider.

The callback does **not** mint the session. It mints a one-use ticket, good for
two minutes, and sends the browser back to the page with it in the URL fragment
(which a browser sends to no server and no access log holds); the page hands it
to `POST /api/v1/session`, which is where every browser session of this server
is made. One place, one policy.

### Mapping and provisioning

The verified address is looked up as a mailbox on this server. When there is
one, that is the session. When there is not:

* with `oidc_auto_provision` **off** (the default), the sign-in is refused. A
  directory of ten thousand people is not ten thousand mailboxes;
* with it **on**, a mailbox is made - but only at a domain this server already
  has, and with a random password nobody is ever told, so until an administrator
  sets one the mailbox can be signed in to through the issuer and no other way.
  No password policy runs on that password, because nobody chose it.

For the Control Deck, the address must be in `oidc_administrators`. **An issuer
that says somebody is an administrator is not obeyed**: what a person may do
here is decided by this server's roles and this server's list.

### What single sign-on does not do

* **No logout, in either direction.** Signing out of the webmail ends the
  session here; the session at the issuer is the issuer's. There is no
  back-channel or front-channel logout, so an account disabled at the issuer
  keeps any session it already started until that session's own ceiling ends it.
* **No claims mapping.** No groups, no roles, no display names. The `email`
  claim and nothing else.
* **One issuer.** An installation fronting two directories needs two, and that
  is not built.
* **The access token and the refresh token are thrown away.** This is a sign-in,
  not a grant to act at the provider on the person's behalf.
* **Nothing is enforced at the protocols.** IMAP, POP3 and SMTP still take the
  account's password (and its application passwords). Single sign-on is the
  *pages*' front door; the bearer tokens those protocols validate
  (`OAuth2Enabled`, see the administrator documentation) are a separate,
  older mechanism with its own settings.

Where it lives
--------------

| | |
|---|---|
| The CBOR, COSE and signature reader | `Common/Util/WebAuthn.{h,cpp}`, with `WebAuthnTester` running fixed vectors on every debug start |
| The credentials and the challenges | `Common/Util/PasskeyStore.{h,cpp}`, table `hm_passkeys` (schema 6077) |
| The REST routes | `Common/Util/RestApiPasskeys.cpp` |
| The OIDC flow | `Common/Util/OidcSignIn.{h,cpp}` and `Common/Util/RestApiOidcSignIn.cpp` |
| The webmail | the Passkeys card on the Security page, and the two buttons on the sign-in card |
| The Control Deck | the Passkeys view, and the two buttons on the sign-in card |
| The fixtures | `RegressionTests/API/RestApiPasskeys.cs` (a software authenticator in the fixture) and `RegressionTests/API/RestApiOidcSignIn.cs` (a fake issuer in the fixture) |

`hm_passkeys` is **not in the backup**, and `AccountStores.cpp` says why: a
credential is bound to the relying party id and to an authenticator that will
sign for that id and no other, so one carried into an archive restored on
another host could never be used there - and the table holds the administrator's
own rows, which are not an account's to carry. Enrolling the device again puts
it back; the password and the one-time code are the fallback meanwhile.

Registering and removing a passkey **is** on the audit trail, as an application
password is: who added a way into a mailbox, and who took one away. The
signature counter written at each sign-in is not, because a sign-in is nobody's
administrative change.

See also
--------

* [DevicesAndSessions.md](DevicesAndSessions.md) - what has signed in as a
  person, which credential each client used (a passkey says `passkey` and a
  single sign-on says `single_sign_on`), how to make one stop, and the session
  policies that govern every session including these.
* [AlertsAndAuditTrail.md](AlertsAndAuditTrail.md) - where the record of a
  passkey added or removed is read.
