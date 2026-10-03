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
* **SAML sign-in** does the same for an identity provider that speaks SAML 2.0
  rather than OpenID Connect - an ADFS farm, or an Entra ID or Okta application
  set up the SAML way. The identity provider's answer is a signed XML document,
  and this server verifies that signature itself: see
  [Single sign-on (SAML)](#single-sign-on-saml) below.

All three are additions. A stock installation is unchanged: passkeys are
offered but nobody has one until they enrol it, and neither kind of single
sign-on does anything at all until an administrator names an identity
provider. The password, and the one-time code where it is enrolled, stay
exactly as they were and are the fallback for everything below.

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
password is refused outright. So, with 409, is the administrator's registration
while the administrator's second factor is enrolled but its secret cannot be
opened on this machine (the key it was protected with is gone): the code cannot
be checked, and a factor that cannot be checked is not taken for one switched off.

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
  loopback name, which is what a browser treats as a secure context) - or, once
  `passkey_allowed_origins` or `passkey_relying_party` is set, one the list names
  (*The relying party id*, below) - and `crossOrigin` is not true;
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
  credential: it exists and is active, **its domain is active**, the
  compromised-account detection is not **holding** the name, and the **domain's
  country policy** allows a sign-in from where this one came. A single sign-on
  ticket meets the same checks, for the same reason: the hold and the country
  policy are what every credential that proves identity without a password
  meets in this tree (a client certificate, a Kerberos ticket, a bearer token on
  IMAP, POP3 and SMTP), the domain is what a password meets, and a credential
  that skipped them would be a way past a lock every other way in obeys. The last-logon stamp moves on here
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

**With `passkey_relying_party` set, `passkey_allowed_origins` must be set too**,
to the origins this server's pages are served at, comma separated and written as
the browser writes them: `https://mail.example.com, https://webmail.example.com`
(with the port where there is one). Until it is, no passkey is registered or
taken, and the Passkeys card and the sign-in say so. The reason: the origin in
the assertion is the one thing about it the browser vouches for, and a
parent-domain relying party is one that a page at **any** host under it may ask
the browser for - `shop.example.com`, a user-content host, anything. Were the
origin only compared with the request's own `Host` header, which the client
writes, a script on any sibling host could collect a real person's assertion
with a normal "sign in to example.com" prompt and post it here with that host in
`Host`, and be signed in as them, with no password and no code. The origin is
therefore held to the list whenever the list is set, and whenever a relying
party is. An assertion or a registration made in a frame of another site's page
(`crossOrigin` true) is refused as well.

With neither set - the default - the relying party id is the `Host` header and
the origin must be that same host. That is safe as it stands: a page at another
host gets a credential for its own name, whose relying party hash no passkey of
this server's carries.

Behind a TLS-terminating proxy, the `Host` header is still what the browser
sent, so the check holds; `X-Forwarded-Proto` is **not** consulted, which is why
the origin check accepts `https` at the page's host and plain `http` only on a
loopback name.

Such a proxy, in front of the plain loopback listener, has to forward a name the
listener answers to - since 6.3.6 it refuses any other with 421, which is what
stops a page that rebinds its own name to this machine. The server's host name
and the hosts of the addresses configured as the webmail's outside address pass,
`passkey_allowed_origins` among them; [DevicesAndSessions.md](DevicesAndSessions.md)
has the list.

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
country policy or past an account or a domain that has been made inactive - see
the sign-in list above. A session, however it was started, ends at its next
request once its account or its domain is switched off. That matters most in the case it was written for: a takeover that
had the password long enough to enrol a passkey of its own, and is then locked
out of the password, the app passwords, IMAP, POP3 and SMTP by the detection.
The passkey it enrolled is refused with them.

The challenge a sign-in answers is handed out to anybody, because a person
signing in with a passkey has no credential to present yet. So it is **not held
here**: the browser carries it in its `hmpasskey` cookie, sealed, and single
sign-on's unfinished sign-ins travel the same way - no table, so nothing a
stranger asks for can push anybody's out (What a stranger can make the server
hold, below). Until 6.3.6 a table dropped its oldest entry whoever's it was, which
a hundred requests a second turned into a switch for both buttons.

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

The ticket is **bound to the browser** that finished the sign-in. The same
redirect sets an `hmsso` cookie (HttpOnly, `SameSite=Strict`,
`Path=/api/v1/session`, two minutes) holding a second random value kept with
the ticket, and `POST /api/v1/session` refuses a ticket that arrives without
it. A ticket on its own is a string, and a string can be sent to somebody else
as a link: without the cookie, a person who completed a sign-in of their own
could send `https://mail.example.com/portal#sso=<ticket>` to someone, and this
server's own page would redeem it in that person's browser and sign them into
the sender's mailbox - login forgery, which neither the `X-Requested-With`
check above nor the SAML `hmsaml` cookie stops, since the post comes from this
server's own page and the SAML check is made in the sender's browser. The
session answer clears the cookie.

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

Auto-provisioning makes a mailbox in passing, for whoever arrives with a verified
address, and never touches it again: it does not rename it, suspend it or remove
it when the person leaves. **SCIM provisioning is the deliberate version** - the
identity provider itself makes, renames, suspends and removes exactly the people
the organisation assigned to this server, with their aliases and the lists they
belong to, over `/scim/v2` under an API key that holds `provision`
([Scim.md](Scim.md)). The two work together: SCIM keeps the mailboxes, and single
sign-on is how the people it made sign in to them.

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

Single sign-on (SAML)
---------------------

The identity providers that still speak only SAML 2.0 - ADFS above all, and an
Entra ID or Okta application set up the SAML way - for the same two pages, ending
in the same ticket, the same `POST /api/v1/session` and the same mapping rules
as OpenID Connect.

**This is an XML-signature feature first and a sign-in feature second.** An
OpenID Connect `id_token` is a JWT whose signature covers its bytes as they
arrive. A SAML Response is an XML document whose signature covers a
*canonical form* of one element of it, and there is no verifying that without an
XML reader that keeps what the canonical form needs, an implementation of
Exclusive XML Canonicalization 1.0, and XML-Signature verification - none of
which this server had. So they were written for this, in
`Common/Util/XmlDocument.{h,cpp}` and `Common/Util/XmlSignature.{h,cpp}`, and are
held to the W3C's own canonicalisation examples and to what .NET's `SignedXml`
writes. Almost every published SAML vulnerability of the last decade was in
exactly that layer rather than in the sign-in logic, which is why most of what
follows is about it.

### Setting it up

| Setting (REST; hm_settings) | What it is |
|---|---|
| `saml_idp_entity_id` (`SamlIdpEntityId`) | The identity provider's entity ID, exactly as its metadata writes it: the `Issuer` of every answer. Empty, the default, offers no SAML sign-in at all. |
| `saml_idp_sso_url` (`SamlIdpSsoUrl`) | Its sign-in URL for the HTTP-Redirect binding. One line, `https://` or the loopback. |
| `saml_idp_certificate` (`SamlIdpCertificate`) | Its signing certificate in PEM - or two, while it rolls its key over. **Write-only**, though it is not secret (below). RSA of 2048 bits or more, or an elliptic curve of 256 or more. A private key anywhere in the paste, anything that is not a certificate, a third certificate, or a paste over 60,000 characters is refused (the gateway-encryption row's `AcceptableKeyText` is the gate), and what is stored - the certificates written out again as PEM, 64 characters to a line, without anything a tool printed around them - must fit the setting's 4,000 characters (below). A paste whose line breaks were dropped or made spaces, which is what the Deck's box does to one, is read as well. |
| `saml_sp_entity_id` (`SamlSpEntityId`) | This server's entity ID as the identity provider knows it; every assertion's audience must name it. Empty means `<base URL>/saml`. |
| `saml_email_attribute` (`SamlEmailAttribute`) | The attribute the address is read from when the NameID is not an email address: `http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress` by default, which is what Entra ID and ADFS send. |
| `saml_require_signed_assertion` (`SamlRequireSignedAssertion`) | Whether the Assertion must carry its own signature. **On** by default; off takes a signature over the whole Response as enough. |
| `saml_auto_provision` (`SamlAutoProvision`) | As `oidc_auto_provision`, for SAML. Off. |
| `saml_administrators` (`SamlAdministrators`) | As `oidc_administrators`, for SAML: the addresses that open the Control Deck. Empty. |
| `saml_button_label` (`SamlButtonLabel`) | The words on the SAML button of both sign-in pages: **Sign in with SAML** by default, which each page says in its reader's language, and anything else shown as written. One line, at most 60 characters. Its own setting, so that the SAML button never reads as the OpenID Connect one does when one identity provider - Entra ID's `login.microsoftonline.com` - serves both. |

**The certificate is never answered back.** Not by `GET /api/v1/settings`,
which answers `saml_idp_certificate_fingerprints` (read-only) instead - each
certificate's SHA-256 fingerprint, its SHA-1 thumbprint and its expiry; not by
the list of what is changed from stock (`GET /api/v1/settings/changed`,
`hmctl config changed`, COM's `Settings.GetChangedFromDefault`), which names it
with `(set)`; and not by the audit trail, which records
`SamlIdpCertificate: (changed)`. The last two follow from its name being
withheld as a secret's is (`AuditTrail::IsSecretName`, by the fragment
`idpcertificate`, beside the gateway key's `encryptkey`), and so does one more
thing worth knowing: `--apply-environment`, given `HM_PROPERTY_SamlIdpCertificate`,
says it set the value and not what the value was.

**What fits in 4,000 characters.** One certificate is stored as its PEM, and
what that comes to was measured: a self-signed 2048-bit RSA certificate, as
identity providers make them, is about 1,200 to 1,470 characters, and a 4096-bit
one about 1,770 to 2,160 - the spread is the subject and the extensions, not the
key. So two 2048-bit certificates fit with room to spare; two 4096-bit ones fit
when they are the ordinary kind (two at 1,900 are 3,800), and two with long
subjects - an Okta-style one with country, state, locality, organisation, unit
and an email address is about 2,150 - do not. The refusal names the stored
length, so there is nothing to work out beforehand.

**A certificate that cannot be read stops every SAML sign-in.** The settings
refuse one, so that is reached only through a database edited by hand: the ACS
then refuses each Response and **HM7320** in the error log says which setting to
paste again.

The base URL is OpenID Connect's: `oidc_redirect_base_url`, falling back to
`import_oauth_redirect_base_url`. From it this server's two addresses follow -
the **entity ID** `<base URL>/saml` and the **ACS URL** (Assertion Consumer
Service) `<base URL>/portal/saml/acs` - and its metadata is at
`<base URL>/saml/metadata`, for an identity provider that imports one. The
settings are REST's and the Deck's (its Server group draws them from the API's
own description), as OpenID Connect's are; there is no COM property for either.
The Control Panel has both, with the passkey settings, on Connections & protocols >
Webmail, the Sign-in tab, which reads and writes them over the REST API, so the
REST API has to be on.

**Microsoft Entra ID.** Enterprise applications, *New application*, *Create your
own application*, "Integrate any other application you don't find in the
gallery". Under *Single sign-on* choose *SAML*. In *Basic SAML Configuration*,
the *Identifier (Entity ID)* is `<base URL>/saml` and the *Reply URL (Assertion
Consumer Service URL)* is `<base URL>/portal/saml/acs` - or upload the metadata
file. *Attributes & Claims* already sends the `emailaddress` claim; its
*Unique User Identifier* is the user principal name in the email-address format,
which is used as the address when it is one, so where people's user principal
names are not their mail addresses, set it to `user.mail`. Under *SAML
Certificates*, download *Certificate (Base64)* and paste it into
`saml_idp_certificate`; leave the signing option at *Sign SAML assertion* (or
*Sign SAML response and assertion*) and the algorithm at SHA-256, and leave token
encryption off. *Set up* the application shows the *Login URL* - that is
`saml_idp_sso_url` - and the *Microsoft Entra Identifier*
(`https://sts.windows.net/<tenant id>/`) - that is `saml_idp_entity_id`. Assign
the people who may use it.

**Okta.** *Applications*, *Create App Integration*, *SAML 2.0*. The *Single
sign-on URL* is `<base URL>/portal/saml/acs`, used for the Recipient and the
Destination as well; the *Audience URI (SP Entity ID)* is `<base URL>/saml`; the
*Name ID format* is *EmailAddress* and the *Application username* is *Email*.
Under the advanced settings Okta signs the Response and the Assertion, both
RSA-SHA256, which is taken as it is, and *Assertion Encryption* must stay
*Unencrypted*. *View SAML setup instructions* shows the *Identity Provider
Single Sign-On URL* (`saml_idp_sso_url`), the *Identity Provider Issuer*
(`saml_idp_entity_id`) and the *X.509 Certificate* (`saml_idp_certificate`).

**ADFS.** In AD FS Management, *Relying Party Trusts*, *Add Relying Party Trust*,
*Claims aware*; import the data from `<base URL>/saml/metadata`, or enter it by
hand - SAML 2.0 WebSSO with the ACS URL above, and `<base URL>/saml` as the
relying party trust identifier. In the claim issuance policy, *Send LDAP
Attributes as Claims* with *E-Mail-Addresses* sent as *E-Mail Address*, which is
the default `saml_email_attribute`; a *Transform an Incoming Claim* rule that
sends it as the Name ID in the *Email* format works as well. ADFS signs the
assertion, with SHA-256, as the default expects, and does not encrypt, because
the metadata offers no encryption certificate. `saml_idp_sso_url` is
`https://<adfs host>/adfs/ls/`; `saml_idp_entity_id` is the *Federation Service
Identifier* (*Service*, *Edit Federation Service Properties*, typically
`http://<adfs host>/adfs/services/trust`); `saml_idp_certificate` is the
*Token-signing* certificate from *Service*, *Certificates*, exported as
Base-64 encoded X.509.

`GET /api/v1/signin-methods` then answers `saml: true` and `saml_label` (the
words of `saml_button_label`), and both sign-in pages draw a second button -
**Sign in with SAML**, unless it is named otherwise - beside the OpenID Connect
one: an installation may front both.

### The flow

    GET  /portal/saml/start[?deck=1]   to the identity provider, with an AuthnRequest,
                                       and the hmsaml cookie
    POST /portal/saml/acs              back from it, with the Response
    GET  /portal/saml/finish           on from the ACS, where the cookie is asked for
    GET  /saml/metadata                this service provider, for the identity provider

The start writes an AuthnRequest - its ID, this server's entity ID as the
Issuer, the ACS URL, the HTTP-POST binding for the answer - compresses it with
raw DEFLATE (RFC 1951, no zlib header), base64s it into the `SAMLRequest`
parameter of the sign-in URL (the HTTP-Redirect binding), adds a `RelayState`,
and sends the browser there. The request is **not signed**: this server holds no
SAML signing key, and the metadata says so (`AuthnRequestsSigned="false"`, no
key descriptor).

**Two bindings: the request, at the ACS, and the browser, a step later.** Nothing
of a start is kept here (What a stranger can make the server hold, below). The
request's ID says itself which page the sign-in is for and that it ends ten minutes
on, under a tag of the server process's - `_<32 hex digits>.<w or d>.<end>.<tag>`,
an `xs:ID` - and the RelayState that goes out beside it is the same ID, within the
80 bytes the bindings allow. A Response is taken only when its `InResponseTo` is
such an ID, in time and not yet answered, and it comes back with the same
RelayState; an ID is spent once a Response to it has verified, so a second
Response to it is refused.

The cookie, `hmsaml`, is `HttpOnly`, `SameSite=Lax`, `Path=/portal/saml` and ten
minutes long, and `Secure` when the request came over TLS, or through a proxy
that says `X-Forwarded-Proto: https` from this machine - the only place a proxy
in front of the REST listener can be, since the listener speaks plain HTTP on the
loopback address alone. It is **not** asked for at the ACS: the identity
provider's POST there is cross-site, so a `SameSite=Lax` cookie does not ride it,
and a `SameSite=None` one needs `Secure`, which a server on plain HTTP to the
loopback cannot promise. It is asked for at the finish, which the ACS sends the
browser on to: a top-level navigation from this server to itself, which a Lax
cookie does ride.

The ACS takes the form the browser posts - `SAMLResponse` and `RelayState` - of
**at most 256 KB**. The REST listener holds every request to 64 KB unless the
route is granted more (`IsLargeRequest_`); this one is granted, and held by
`LargeRequestCeiling_` to its own 256 KB rather than the 16 MB the attachment
routes are granted, and to the ordinary request timer. A larger body is answered
413 before anything reads it. A signed Response from Entra ID, Okta or ADFS is a
few kilobytes; the fixture signs one of about 170 KB on the wire, which only the
route's own ceiling lets through and which signs in, and posts 300 KB and 1.1 MB,
which are refused.

Then the Response is verified (below). What was verified - the address, which
page the sign-in was for and the request it answered - is kept under a one-use
token of 32 random bytes, for two minutes, and the ACS sends the browser (303, since it came with a
POST) to `/portal/saml/finish?token=`. The finish spends the token whatever
happens next, and goes on only when the browser carries the `hmsaml` cookie the
start gave for that very request (compared in constant time); it clears the
cookie. Only then is the address mapped - so no mailbox is made for a Response
that ends in the wrong browser - and, exactly as OpenID Connect's callback does,
a one-use ticket good for two minutes is minted and the browser sent to the page
with it in the URL fragment, with the `hmsso` cookie that binds it to this
browser; the page hands it to `POST /api/v1/session`.

A refusal comes back as `#sso_error=` with the sentence, to the page the sign-in
was started for. **That page is chosen by this server alone**: at the ACS it is
the one the posted RelayState's own sign-in named, and at the finish the one the
verified sign-in named; a Response naming another sign-in in its `InResponseTo`
does not choose it. The Control Deck and the webmail show the sentence, once and
inside words of their own, only in the browser tab whose sign-in card sent it to
the identity provider in the last hour and has not loaded either page since; any
other `#sso_error=` is taken out of the address and not shown, and the page loads
as on any other visit. An hour rather than a sign-in's ten minutes, since a
slower return is answered with this server's own sentence that the sign-in took
longer than ten minutes. The two pages share the tab's mark, since a sign-in
this server no longer knows is sent to the webmail whichever page started it.

**Two sign-ins in one browser.** A second start replaces the cookie, so the
first sign-in, if its Response still arrives, is refused at the finish; start
again. The expiries - ten minutes for a request, two for the finish token - are
read in the code and not tested, since a test would have to wait them out; the
eviction of the oldest of two hundred is tested.

### What is verified

The signature, in `XmlSignature::Verify`:

* exactly one `ds:Signature` among the signed element's children - an
  **enveloped** signature - and no signature anywhere else in the document but
  the Response's own and the Assertion's own;
* `SignedInfo` canonicalised with Exclusive XML Canonicalization 1.0 without
  comments (its `InclusiveNamespaces` PrefixList honoured); inclusive
  canonicalisation, and exclusive canonicalisation **with comments**, refused;
* the signature method `rsa-sha256` or `ecdsa-sha256`; SHA-1 and HMAC refused;
* exactly one `Reference`, whose URI is `#` and the signed element's own `ID` -
  an empty URI, which is the whole document, or any other element refused - and
  that `ID` written once in the whole document;
* the transforms **exactly** `enveloped-signature` and then exclusive
  canonicalisation; XSLT, XPath, base64 and anything else refused;
* the digest SHA-256, over the element as that canonicalisation writes it with
  the signature left out; SHA-1 refused;
* the signature against **the certificates an administrator stored**, and never
  a key the message carries: `KeyInfo` is ignored - and checked **first**, over
  the canonical `SignedInfo`, before the element the Reference names is
  canonicalised and its digest compared, so that of a document the stored keys
  did not sign nothing but its `SignedInfo` is canonicalised;
* base64 read strictly, so that a value has one spelling;
* and then **the anti-wrapping rule**: what is used is the signed bytes read again
  as a document of their own, never the element looked up again in the
  original.

The document's shape, before any of that: one `Assertion`, directly under the
Response; its `ID` and the Response's written once each in the whole document;
an `EncryptedAssertion` refused with a sentence that says to switch encryption
off at the identity provider; the status `Success`, and otherwise the identity
provider's own reason (`AuthnFailed`, `RequestDenied` ...) in the sentence.

The Assertion must carry its own signature while `saml_require_signed_assertion`
is on - the default, and how Entra ID and ADFS sign. Off, a signature over the
whole Response is enough, which is what an identity provider set to sign the
response only sends; the Assertion used is then the one inside the Response's
signed bytes. When both are signed, both must verify, and the Assertion used is
the one its own signature covered.

Then SAML 2.0 core and the Web Browser SSO profile's section 4.1.4.3:

* the Response's `Destination` is this server's ACS URL, its `InResponseTo` a
  request issued here and not yet answered, coming back with its RelayState,
  and its `Issuer`, when it has one, the identity provider's entity ID;
* the Assertion's `Issuer` is the identity provider's entity ID;
* a **bearer** `SubjectConfirmation` whose `SubjectConfirmationData` has the ACS
  URL as its `Recipient`, the same request as its `InResponseTo`, and a
  `NotOnOrAfter` in the future (and a `NotBefore`, when it has one, in the past);
* `Conditions`' `NotBefore` and `NotOnOrAfter`, with a minute of skew;
* an `AudienceRestriction` naming this server's entity ID - every one of them,
  where there are several;
* an `AuthnStatement`;
* the Assertion's `ID` not taken before: it is kept in memory until the
  assertion expires or five thousand newer ones push it out, and forgotten when
  the REST listener stops - by which time every request ID it could answer has
  been forgotten too.

The address is the `NameID` when its format is
`urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress`, and otherwise the
attribute `saml_email_attribute` names, which must carry exactly one value.

### Mapping and provisioning

Exactly OpenID Connect's (above), through the same function
(`OidcSignIn::MapAddress`), with `saml_administrators` in place of
`oidc_administrators` and `saml_auto_provision` in place of
`oidc_auto_provision`: the mailbox at the address, active; with provisioning on,
a mailbox made at a domain this server already has and with a password nobody is
told; an address at a domain this server does not have refused; and the Control
Deck only for an address the list names. An identity provider that says somebody
is an administrator is not obeyed.

**A SAML identity provider says nothing like `email_verified`.** The address it
sends is taken as the directory's, so the attribute it is read from must be one
the directory controls - `mail` or the user principal name, which people cannot
edit - and never one a person can set at the identity provider themselves, or
one person can name another's mailbox. That is the identity provider
administrator's to decide, and it is said here because this server cannot check
it.

### What SAML sign-in does not do, and what that costs

* **No single logout.** Signing out of the webmail ends the session here; the
  session at the identity provider is the identity provider's, and a person
  disabled there keeps a session already started here until its own ceiling
  ends it - as with OpenID Connect.
* **No encrypted assertions.** An `EncryptedAssertion` is refused, with the
  sentence that says so. The assertion therefore passes through the browser
  readable; TLS protects it on the wire, and since it arrives in a POST body no
  access log holds it, but the browser - and anything running in it - can read
  it.
* **No signed requests.** The AuthnRequest is not signed, so an identity provider
  set to demand signed requests cannot be used, and anybody can write a request
  naming this server - which gains nothing, since the Response must answer a
  request this server issued.
* **No metadata fetch.** The three values are pasted, not read from the
  identity provider's metadata URL; when it rolls its key over, the new
  certificate has to be pasted beside the old one before the switch, and the old
  one taken away after.
* **No artifact binding, no attribute queries, and no IdP-initiated sign-in.** A
  Response nobody asked for - the identity provider's own portal sending people
  to the ACS - is refused with a sentence: this server takes only the sign-ins it
  started, and a person starts one from the sign-in page.
* **One identity provider**, and one OpenID Connect issuer beside it.

### The threat model

* **Signature wrapping.** The attack moves the signed Assertion somewhere a
  verifier looks and puts a forged one where the application looks. Here the
  application only ever looks at the signed bytes read back; there must be one
  Assertion, where SAML puts it, its `ID` written once; the Reference must name
  the signed element's own `ID`; and a signature anywhere but on the Response or
  the Assertion is refused. The fixture tries the attack with the forgery under
  the same `ID` and under another, and both are refused by the first of those
  rules to meet them, the count of Assertions; that what is used is the signed
  bytes read back is held by `XmlSignatureTester`, which no document sent from
  outside can reach past the shape checks.
* **Replay.** A request ID is spent by its first Response, so a Response posted
  twice is refused by that; and an Assertion's `ID` is kept (above), so the same
  assertion in a Response to a second, fresh sign-in is refused too - the fixture
  proves both.
* **Comment injection** (the 2018 family of attacks). A `NameID` of
  `user@example.test<!---->.evil` carries a signature over
  `user@example.test.evil`, because canonicalisation without comments leaves the
  comment out. The reader drops comments and joins the text either side into one,
  so the address read is `user@example.test.evil`, the whole of what was signed,
  and not the `user@example.test` a reader that stops at the first text node
  would take.
* **The reader's refusals.** No document type declaration, so no entity - no
  external entity to read a file with and no nested entities to exhaust memory
  with - and no entity but the five XML predefines; no processing instruction;
  UTF-8 only, every character checked, a surrogate refused; an attribute written
  twice, or once under each of two prefixes, refused; a prefix nobody declared
  refused, the `xml` prefix held to its own namespace and no prefix bound to the
  `xmlns` one; nesting to 64, 20,000 elements, 256 attributes on one element, 128
  namespace declarations, namespace names to 512 characters and a document to
  1 MB (the ACS's own ceiling, 256 KB, is below that) - each ceiling held by the
  tester on both sides of it.
* **Algorithm confusion.** The key is the stored certificate's, and its type
  must be the signature method's: an ECDSA signature is not checked against an
  RSA key, nor the reverse, and RSA under 2048 bits is not used.
* **Login forgery, closed by the finish.** A Response must answer a request
  this server issued, with that request's RelayState, so a Response nobody asked
  for is refused, and so is one for another sign-in. What those two cannot see is
  a person who starts a sign-in *themselves*, keeps the Response their own
  identity provider sends back, and has another person's browser post it: the
  request and the RelayState are both right. The finish is what refuses that -
  the other browser does not hold the cookie the start gave for that request -
  and the fixture posts a Response started in one browser and finishes it in
  another, with that browser's own cookie and with none, beside Responses
  finished in the browsers that started them. What is left needs more than a
  Response: a person who can set cookies for this server's host - from a sibling
  subdomain serving pages they control, which can toss a cookie of its own at
  `Path=/portal/saml` - could plant their own `hmsaml` beside a Response of their
  own. That is the reason not to serve anybody else's pages from a name that
  shares the webmail's parent domain.
* **Floods.** The start carries no credential, so its table evicts rather than
  refuses, and so does the finish's; the ACS is held to 256 KB and the ordinary
  timer. The canonicaliser, which runs on a document before any key has been
  checked, is bounded where its work grows with what a sender controls: the
  reader refuses a document declaring more than 128 namespaces (a SAML document
  declares fewer than twenty) or a namespace name over 512 characters (exc-c14n
  writes a declaration again on every element that uses a prefix no output
  ancestor wrote, so one long name used by many elements is written many
  times), and an `InclusiveNamespaces` PrefixList is read without its repeats and
  refused past 32 prefixes; an element's scope is copied only when it declares
  something, and what the output ancestors wrote is never copied; and the
  signature is checked over `SignedInfo` before the signed element is
  canonicalised at all. `XmlCanonicalizationTester` holds a worst-case document
  of 256 KB - every ceiling spent where it costs most - to a time bound.

What a stranger can make the server hold
---------------------------------------

A passkey sign-in's challenge (`POST /api/v1/passkeys/challenge`), and the start
of an OpenID Connect or SAML sign-in (`/portal/oidc/start`, `/portal/saml/start`),
are handed to anybody - nobody has signed in yet, which is the point. **None of
them is held on the server.** What the sign-in needs when the browser comes back
travels with the browser, in a cookie, sealed with AES-256-GCM under a key of the
server process's (`Common/Util/SignInState.h`):

| Cookie | What it holds, sealed | Where it is asked for |
|---|---|---|
| `hmoidc` | the state, the PKCE verifier, the nonce, which page, when it ends (ten minutes) | the callback |
| `hmsaml` | the request's ID | the finish |
| `hmpasskey` | the challenge, when it ends (five minutes) | `POST /api/v1/session` |

The browser cannot read or change what it carries: a cookie that does not open, or
that opens to another sign-in than the one the identity provider's answer names,
is refused. A SAML request's ID reaches the ACS through the identity provider
rather than in the cookie (a `SameSite=Lax` cookie does not ride the provider's
cross-site POST), so it carries its own tag instead, as above.

So no number of starts or challenges asked for - from one address, from a
thousand, from a rotating proxy pool's tens of thousands - pushes anybody's sign-in
out: there is no table to fill. Until 6.3.6 the tables held two hundred (OpenID
Connect, SAML) and a thousand (passkeys) and dropped the oldest whoever held it, so
a stranger asking a few times a second - a hundred a second for passkeys - made
every other person's sign-in fail when their browser came back ("this sign-in was
not started here"), for as long as they kept asking; as 6.3.6 was first written
each source was held to its own share of a table of 4,096, which a pool of
addresses as large as the table still beat.

**What is kept: that a sign-in finished.** Each finished sign-in is spent - its
OpenID Connect state once the issuer's `id_token` has verified, its SAML request
once a signed Response to it has, its passkey challenge once an assertion over it
has - and remembered until it would have expired, so that it is good once. Only a
finished sign-in adds to that set, which a stranger cannot do without signing in;
it is bounded all the same, at 16,384 of each kind.

**The key** is 32 random bytes made when the server first needs it, and made again
whenever the REST listener stops (a reinitialise, a stop), held in memory and
written nowhere - the lifetime the tables had: a sign-in in flight across a restart
never survived it, and does not now ("this sign-in was not started here; start
it again"). A key kept longer would let a state sealed before the restart, whose
spent record went with it, be used a second time. Two servers behind one name each
seal with their own, as each had its own table: the browser has to come back to
the one it started at. `X-Forwarded-For` is not read for any of this, so a reverse
proxy needs no setting for it.

The registration challenge, which needs a session, is held against the session
that asked and counts against the credential that asked: fifty a credential, in a
table of a thousand, under the rule the browser sessions follow
([DevicesAndSessions.md](DevicesAndSessions.md), How many sessions, and who makes
room) - so that an account asking from session after session, or from address
after address, spends its own registrations and not another person's.

Where it lives
--------------

| | |
|---|---|
| The CBOR, COSE and signature reader | `Common/Util/WebAuthn.{h,cpp}`, with `WebAuthnTester` running fixed vectors on every debug start |
| The credentials and the challenges | `Common/Util/PasskeyStore.{h,cpp}`, table `hm_passkeys` (schema 6077) |
| The sealed state of the sign-ins in flight | `Common/Util/SignInState.{h,cpp}`, with `SignInStateTester` (ClassTester on Windows, `sign-in-budget-tests` on Linux) |
| The REST routes | `Common/Util/RestApiPasskeys.cpp` |
| The OIDC flow | `Common/Util/OidcSignIn.{h,cpp}` and `Common/Util/RestApiOidcSignIn.cpp`; `OidcSignIn::MapAddress` is the mapping both kinds of single sign-on share |
| The XML reader and the canonicaliser | `Common/Util/XmlDocument.{h,cpp}`, with `XmlCanonicalizationTester` holding them to the W3C's examples |
| XML-Signature verification | `Common/Util/XmlSignature.{h,cpp}`, with `XmlSignatureTester` verifying vectors .NET's `SignedXml` signed and refusing every single-byte change to them |
| The SAML flow | `Common/Util/SamlSignIn.{h,cpp}` (the start, the ACS's verification and the finish) and `Common/Util/RestApiSamlSignIn.cpp` (the routes and the `hmsaml` cookie); the ACS's 256 KB ceiling is `RestApiServer::LargeRequestCeiling_` over `HttpServer`'s per-route ceiling |
| The webmail | the Passkeys card on the Security page, and the three buttons on the sign-in card |
| The Control Deck | the Passkeys view, and the three buttons on the sign-in card |
| The fixtures | `RegressionTests/API/RestApiPasskeys.cs` (a software authenticator in the fixture), `RegressionTests/API/RestApiOidcSignIn.cs` (a fake issuer in the fixture) and `RegressionTests/API/SamlSignIn.cs` (a SAML identity provider in the fixture, signing with .NET's `SignedXml`) |

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
