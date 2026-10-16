The web surface: a threat model
===============================

**Written 12 October 2026, and brought up to date with the tree at 6e1a336c5 on
15 October 2026,** for row A1 of Programme A, the rebuild of the Control Deck and
the webmail planned for 6.4 ([ControlDeckFramework.md](ControlDeckFramework.md)).
It describes the web surface as it is today and as it will be after the
rebuild: what is worth protecting, who might attack it, every way in, what
protects each today (with the file and line), and where the protection is thin.
The route-by-route authorisation is [WebAuthorisationMatrix.md](WebAuthorisationMatrix.md),
written from the code by `build/generate-auth-matrix.py`; "The routes the matrix
flags, judged", under Findings, judges the routes that file flags.

File and line references are to 6e1a336c5. Source files are under
`hmailserver/source/Server/Common/Util/` unless another directory is named: a
directory named is under `Server/Common/`, but `SMTP/`, `ExternalFetcher/` and
`COM/`, which are under `Server/`.

Scope
-----

Four listeners speak HTTP:

| Listener | Code | Off by default? | What it serves |
|---|---|---|---|
| **The REST listener** | `RestApiServer` on `HttpServer` (RestApiServer.cpp:1241 `Start`, :1343) | Yes: `RestApiPort=0`; bound to `127.0.0.1` unless configured; refuses to start without TLS anywhere but loopback (RestApiServer.cpp:1265) | The Control Deck, the webmail and portal, the REST API, SCIM, and the public pages a link in a mail opens |
| **The web services listener** | `WebServicesServer` on `HttpServer` (WebServicesServer.cpp:570, :651) | Yes: `WebServicesPort` and `WebServicesHttpsPort` both 0 | MTA-STS, autoconfiguration, autodiscover, the Web Key Directory, the S/MIME authority's root and CRL, ACME challenges, security.txt, CalDAV and CardDAV |
| **The ACME responder** | `AcmeChallengeServer`, its own accept loop (AcmeClient.cpp:1669-1862) | Runs only while a certificate is being validated, and only when the web services listener does not hold `AcmeHttpPort` (AcmeClient.cpp:1032-1036) | The HTTP-01 challenge answer: it binds the wildcard address of `AcmeHttpPort` (:1686-1716) and answers the one token, 404 otherwise. Each connection is served on a thread of its own, held to 5 seconds, and the accept loop never waits on one; past 64 at once a connection is closed unanswered (:1763, :1767-1810, :1813-1862; W-11) |
| **The metrics listener** | `MetricsServer`, its own loop (MetricsServer.cpp) | Yes | Prometheus metrics; not a page, not a browser's. Its own bearer token or Basic credential, and 503 to anyone off loopback when neither is set (MetricsServer.cpp:375-449). Outside this model beyond this row |

SMTP, IMAP, POP3 and ManageSieve are outside this model, except where a web
route reaches the same data. The requests the server itself makes to an address
somebody chose are in it (Entry points, "Outbound requests").

Assets
------

| ID | Asset | Where it lives | Why it matters |
|---|---|---|---|
| A-1 | The administrator password and its second factor | hMailServer.ini `[Security]`, hashed (PBKDF2); TOTP secret protected at rest | The whole server: every route, every setting, every mailbox |
| A-2 | API keys | The key store beside the ini, as SHA-256 of the token (`ApiKeyRecord`, RestApiServer.h) | What the key's role reaches; a full key with no list of domains is the server administrator, and one issued for named domains is `serveradmin` confined to them, which is refused `administer_server` (`RoleManager::Allows`) |
| A-3 | Browser sessions | `hmailsession` cookie (RestApiServer.cpp:14866) and the bearer form; the server keeps a hash of the token | The credential that made them, for up to 12 hours |
| A-4 | Account credentials | Passwords hashed in the database; app passwords; passkeys; recovery codes | One mailbox, or an administrative role an account holds |
| A-5 | Mail | The message store and the database | The reason the server exists; what an attacker who can only send mail wants to read |
| A-6 | OpenPGP and S/MIME private keys | Generated or opened in the page; stored on the server encrypted under a passphrase the server never sees (docs/OpenPGP.md:17, :806); zero-access wraps (docs/ZeroAccess.md) | Decrypting and signing as the person; an XSS in the webmail while a key is unlocked is the one way to them |
| A-7 | Server secrets | The `[Settings]` values, stored in the database (`hm_inisettings`) with a copy in hMailServer.ini: the OAuth2 HMAC secret, the password pepper, the service account's password, `AutoBanCommand`; the VAPID key pair | Server compromise; `AutoBanCommand` runs as the service |
| A-8 | The audit trail | The database, hash-chained | Accountability; an attacker who can rewrite it hides |
| A-9 | Capability tokens | In links: `/files/{token}`, `/confidential/{token}`, `/portal/protected/{token}`, `/held/release?t=`, `/lists/...?t=`, `/recovery/verify?t=`, `/portal/reset?t=`, the migration sign-in key | Each is the whole of the authority on its page |
| A-10 | The pages themselves | The Deck read from `<ProgramFolder>/WebAdmin` on every request; the webmail compiled in | Whoever changes the page's script runs as everybody who opens it |
| A-11 | Availability | One process, four REST worker threads (RestApiServer.cpp:230) | Mail is the business |

Actors
------

| ID | Actor | Starts with | Wants |
|---|---|---|---|
| T-1 | Anyone on the internet | Network reach to the listeners | Any of the above |
| T-2 | **A sender** - the owner's own reason for the rebuild | The ability to send a message to an account | To run script in the webmail as the reader, or in the Deck as the administrator (a message, an attachment, a display name, a calendar invitation, a contact card) |
| T-3 | A malicious or compromised account holder | Their own credentials | Another account's mail; administration |
| T-4 | One tenant's domain administrator, help desk or one-domain API key | A role confined to named domains | Another tenant's data or mail, or server-wide authority: through a route that names no domain, or through what the body of a request on its own domain's route names beyond that domain |
| T-5 | An auditor or a read-only key | Every read | A write |
| T-6 | A web site the administrator or a user visits | A page in their browser | To act with their cookie or cached credentials (CSRF), to reach the loopback listener (DNS rebinding), to frame the pages |
| T-7 | A local process or user on the server | Write access to some folder | The Deck's file, the catalogues, the ini |
| T-8 | A network attacker | A position between browser and server | Credentials on plain HTTP; a downgraded profile |
| T-9 | A hostile or compromised identity provider (OIDC, SAML) | A signed assertion | A session as somebody else, or as the administrator |
| T-10 | **The npm supply chain** (new with the rebuild) | A release of a package the build installs | Code in the shipped bundle, or in the build |

Entry points
------------

Every route the two web listeners answer is in
[WebAuthorisationMatrix.md](WebAuthorisationMatrix.md), read from the code:

- **41 routes before any credential** (`ProcessRequest_`, RestApiServer.cpp:2493-2723), in four kinds:
  - the pages and their static files: `/`, `/index.html`, `/deck-lang/*.json`,
    `/portal`, `/portal*.js`, `/portal.webmanifest`, `/portal-sw.js`,
    `/portal-lang/*.json`, `/portal-protected.js`, `/api/v1/help` and its pages,
    `/api/v1/portal/branding`, `/api/v1/signin-methods`, `/saml/metadata`;
  - **capability-token pages** (A-9): `/files/{token}`, `/confidential/{token}`,
    `/portal/protected/{token}` with `/api/v1/protected/{token}` and its reply,
    `/held/release`, `/held/stop`, `/lists/confirm`, `/lists/unsubscribe`,
    `/lists/approve`, `/recovery/verify`, `/portal/reset`,
    `/api/v1/password-reset/complete`, `/portal/oauth/migration`;
  - **sign-in flows**: `/portal/oidc/start` and `/callback`, `/portal/saml/start`,
    `/acs` and `/finish`, `/portal/oauth/callback`, `POST /api/v1/passkeys/challenge`,
    `POST /api/v1/password-reset`;
  - **public list pages**: `/lists/{list}/subscribe` and `/archive`.
- **609 rows (method, path and route kind) of 601 route kinds** decided by `Authorize_` (RestApiServer.cpp:9986,
  called at :2917), each with what nine kinds of credential get.
- **20 routes on the web services listener**, of which CalDAV, CardDAV and
  autodiscover take an account's password over HTTPS (`Dav::Authenticate`) and
  the rest are public by design.
- **Uploads**: every request body. The REST listener takes 64 KB
  (RestApiServer.cpp:214), 16 MB in five minutes for the routes
  `IsLargeRequest_` names (RestApiServer.cpp:218-219, :20853-20917: sending, drafts,
  file and protected-message content, settings, directory entries, OpenPGP
  rings) and 256 KB for the SAML ACS (:226). The web services listener takes
  64 KB, and about 1 MB for CardDAV (WebServicesServer.cpp:67, :78).
- **The browser side**: the Deck's and the webmail's scripts, the service worker
  (`/portal-sw.js`, scope `/`), the share target (`POST /portal/share`, answered
  by the worker), the message-HTML frame, the PDF preview frame, IndexedDB and
  `localStorage` for offline mail, and the OpenPGP and S/MIME modules.
- **Outbound requests**: every address the server sends a request to because
  somebody chose it. `HttpsClient` has three modes: `Request` (any address;
  plain http only to loopback), `RequestNotLinkLocal` (no link-local or metadata
  address) and `RequestPublic`/`RequestPublicThroughProxy` (each address the name
  resolves to must be public, and the proxy, when one is set, is asked for a
  tunnel to an address that passed) - `IsPublicAddress`, HttpsClient.cpp:488,
  and `Connect_`, :649-681.

  | The address | Who chooses it | The rule | Where |
  |---|---|---|---|
  | A message's one-click `List-Unsubscribe` URL, posted when the reader clicks unsubscribe | The sender | Public only | RestApiMessageReplies.cpp:334-350 |
  | An account's Web Push endpoints | The account (its browser's subscription) | Public only; loopback only under the regression suite's switch | PushNotifier.cpp:368-375 |
  | OpenPGP key lookups in an address's Web Key Directory | The address looked up | Public only; the keyserver, a URL an administrator set, by `Request` | OpenPgpRing.cpp:216-256 |
  | An account's IMAP or POP3 fetch host | The account, in the webmail, or an administrator | A fetch account the reader made is refused private addresses unless `ReaderFetchAccountsPrivateAddresses` is set | RestApiFetchAccounts.cpp:592, :870; ExternalFetcher/ExternalFetch.cpp:91 |
  | An inbound webhook route's URL | The credential that made the route | Public only for any credential but the server administrator's, whose routes may reach this network but no link-local or metadata address; no redirect followed | InboundWebhooks.cpp:2239-2248 |
  | The domain's relay host, port and credentials (`relay_host`, `relay_username`, `relay_password`) | The domain's administrator, a confined credential included | The SMTP client. A host set by a credential that is not the server administrator's must be one every address of which is public (`HttpsClient::HostIsPublic`), as the reader's fetch host and the tenant's webhook are; only a change of the host is judged, so a domain whose server administrator set an internal smart host stays its administrator's to save (W-10) | RestApiAdministration.cpp:364-367, :751-759, :1022-1040; COM/InterfaceDomain.cpp:228-261; SMTP/ServerTargetResolver.cpp:250-262 |
  | The portal's brand logo | An administrator | Public only | BrandLogo.cpp:448 |
  | MTA-STS policies | The recipient's domain | Public only | SMTP/TlsPolicy.cpp:455 |
  | Event webhooks | The administrator password, or a role that reaches every domain and holds `manage_server`; no API key | Any address, by design (Webhooks.md) | WebhookManager.cpp:1076-1084 |
  | Migration sources | A credential that reaches every domain (a confined one cannot create a migration, and the address is the migration's) | Any address | MigrationSources.cpp:248; RestApiMigrations.cpp:512-568 |
  | OIDC discovery and its JWKS, token introspection, the OAuth2 token endpoints, the ACME directory, the sandbox, the object store, alert webhooks | The server's administrator, in the settings | Any address | OidcSignIn.cpp:240, :598; JwksKeySet.cpp:182; TokenIntrospection.cpp:142; OutboundOAuth2TokenClient.cpp:221; ReaderOAuth.cpp:151; AcmeClient.cpp:563; AntiVirus/Sandbox.cpp:1016, :1078; Persistence/MessageStoreObjectClient.cpp:621; AlertManager.cpp:968 |

  A4's jobs test that each address of the first five rows is refused where
  their rule says so - a private, loopback, link-local or metadata address -
  with the proxy and without it (ControlDeckFramework.md, A4).

Controls in place today
-----------------------

### Transport and origin

| Control | Where |
|---|---|
| TLS required except on loopback | RestApiServer.cpp:1265 |
| HSTS (one year, the name used, no includeSubDomains) on every response over TLS | RestApiServer.cpp:1368 |
| `X-Forwarded-Proto` believed only from a loopback peer | `BrowserCameOverTls_`, RestApiSamlSignIn.cpp:84-100 |
| The Host header checked before anything is served (421): closes DNS rebinding of the loopback console | `HostAllowed_`, RestApiServer.cpp:2230; asked at :2493 |
| Chunked bodies refused (411); a NUL in a text body refused (400) | HttpServer.cpp:776-791 |
| Absolute time limits per request and per connection, 64 connections | RestApiServer.cpp:227-229 |
| Apple profiles (which carry a password) only over HTTPS | WebServicesServer.cpp:767-779 |

### Authentication

| Control | Where |
|---|---|
| One 401 for every credential problem; `WWW-Authenticate: Basic` left out for a page's own fetch | RestApiServer.cpp:10665-10701; `IsPageScriptRequest_` :10649 |
| The administrator password: PBKDF2, a growing per-source delay that refuses before checking (from loopback too), the second factor in `X-hMailServer-OTP`, each code taken once, five wrong codes lock for fifteen minutes | `AuthenticateBasic_` :1830-1923; throttle :2758-2787 |
| API keys: `hmapi_` tokens stored as SHA-256; source address and expiry; a key cannot make a session, a passkey or another key | :1924; `Authorize_` :9997-10026, :10091-10145 |
| Sessions: `HttpOnly; SameSite=Strict`, `Secure` over TLS; 30 minutes idle, 12 hours at most; 50 per principal; ended by a password change | :14866; the policy in docs/DevicesAndSessions.md |
| Bearer sessions: accounts only, never the administrator, never administrative | `HandleSessionCreate_` :14637 (the OpenAPI text at :21080 says each rule) |
| Passkeys, OIDC and SAML: sealed cookies bind each flow to the browser that started it; a one-use ticket and an `hmsso` cookie for the exchange; `X-Requested-With` on the exchange, so a cross-site form cannot plant an attacker's session (login CSRF) | RestApiPasskeys.cpp:518; RestApiOidcSignIn.cpp:93; RestApiSamlSignIn.cpp:119; RestApiServer.cpp:1664-1684 |
| A request budget per credential: 200 in 10 seconds | :316-317, :2861 |
| The auto-ban on repeated failures | `RegisterAuthenticationFailure_` :2383 |

### Authorisation

| Control | Where |
|---|---|
| One choke point: every route is a `RouteKind` decided by `Authorize_` before any handler runs | :2885-2917, :9986 |
| Every `RouteKind` classified; the auditor reads everything and changes nothing; the decision functions exist | `build/check-role-choke-point.py` |
| One decider of what a role may do. `administer_server` is the whole server only for a role that reaches every domain: one holding it with a list of domains - a full key issued for named domains, `serveradmin` assigned with a list - is confined to them as any confined role is and is refused `administer_server` itself; REST's `HoldsServerAuthority_` and COM's `GetIsServerAdmin` both read that answer | `RoleManager::Allows`, Application/RoleManager.cpp:798 (at 6e1a336c5); a grant with a list is confined at :873 and :890 from 3d01598eb |
| A domain's administrator cannot change an account wider than itself (the server administrator's own mailbox in its domain) | `ChangesAccountByAddress_` :8613, asked at :2949 |
| What is the server's and not a domain's is refused a credential confined to named domains, whatever its role's permissions: the delivery queue, the message store, the quarantine, an inbound webhook pass, the report's server-wide sections, and every resource of the server's own - IP ranges, certificates, global rules, the logs, the backup, the server settings and the update check among them; the creation, rewriting and deletion of a custom role, since a role is assigned across domains (W-1); the metric history, counted from counters that carry no domain (W-3); and a new domain alias, which names a domain beyond the one the path names (W-10). The roles and their assignments stay readable, the assignments narrowed to the credential's domains | `Authorize_` :10203-10603 (W-1 :10282-10295, W-3 :10297-10308, W-10 :10310-10322); RestApiRoles.cpp:230-234 |
| What a body names beyond the path's domain is the handler's to hold, since `Authorize_` sees the path only: an alias's name, a new account's address, a list's address and a mailbox move's target are held to the domain (RestApiRoutes.cpp:1185-1189; RestApiServer.cpp:11968-11971; RestApiAdministration.cpp:1423-1426; RestApiMailboxMove.cpp:94-99), and a change on a domain's path whose handler is not given the caller is flagged by the matrix (`F-write-blind`) | The handlers named; "The routes the matrix flags, judged", below |
| What only the server's authority may set, held by the handler, which is given that authority (W-10): a domain's name - a rename decides what name the server accepts mail for - and, on a domain's or an account's rule, the actions that reach past the rule's owner into the server's configuration: `bind_to_address`, `send_using_route` and `script_function` (a global rule is the server administrator's already). The COM API holds the same for an administrator who is not the server's: `Domain.Name`, `DomainAlias.Save`, `Rule.Save` and `RuleAction.Save` | RestApiAdministration.cpp:1011-1020; RestApiRules.cpp:937-969, :1038, :1097, `RuleGuard::ActionReachesBeyondRuleOwner` Rules/RuleGuard.cpp:622-638; COM/InterfaceDomain.cpp:115-121, COM/InterfaceDomainAlias.cpp:116-124, COM/InterfaceRule.cpp:368-378, COM/InterfaceRuleAction.cpp:74-85 |
| An app password reaches no route of a mailbox encrypted at rest | :2982-2989 |
| Folder rights decided in one place | `ACLManager`; `build/check-authz-choke-point.py` |
| The matrix of every route against every credential, regenerated and compared in CI | `build/generate-auth-matrix.py --check` |

### Cross-site request forgery

| Control | Where |
|---|---|
| The session cookie is `SameSite=Strict` | :14866 |
| A write on a session (cookie or bearer) must carry `X-Requested-With: hMailServer`, which another origin cannot add without a preflight the server never grants | :2789-2803 |
| A credential in a body (passkey, SSO ticket) is accepted only with the same header | :1664-1684 |
| A request that changes something and carries any credential - a session, HTTP Basic, which a browser keeps once it has answered the sign-in box, or a bearer token - is refused when the browser reports it as sent from another site: a `Sec-Fetch-Site` of `cross-site` or `same-site`, or, from a browser that sends no fetch metadata, an `Origin` that names another host than the request's own or names no host. A script, `hmctl` and the PowerShell module send neither header and pass; the Deck and the webmail act on their own origin (W-2) | `RequestIsCrossSite_` :1526-1580, asked at :2835-2841 |
| The anonymous pages that act on a capability token (`/files/{token}`, `/confidential/{token}`, `/recovery/verify`, `/portal/reset`, `/held/release` and `/held/stop`, `/lists/confirm`, `/lists/unsubscribe`, `/lists/approve`) post back to themselves, each held by its token, and change nothing on GET but what the table of flagged routes says | the handlers of each page; the matrix's `F-anonymous-get` |
| The two anonymous posts that carry no token - the public subscribe form (`POST /lists/{list}/subscribe`) and the reset request (`POST /api/v1/password-reset`) - are held by double opt-in, by one answer for every address, by a budget per source address, and by the same rule as a credentialed change: a post the browser reports as sent from another site is refused before its handler. The subscribe form alone takes an `Origin` that names no host, which a browser that sends no fetch metadata may send with this server's own form, as it is served under `Referrer-Policy: no-referrer` (W-12) | RestApiServer.cpp:2650-2665, :2700-2717; RestApiMailingLists.cpp:898-937; RestApiAccountRecovery.cpp:669-698 |

### Script in the pages (T-2)

| Control | Where |
|---|---|
| The Deck: one inline script allowed by its SHA-256, no `'self'`, `trusted-types default`, `frame-ancestors 'none'`, `form-action 'self'`, `base-uri 'none'`, COOP, nosniff, no-store | RestApiServer.cpp:11675-11681 |
| The Deck's `default` Trusted Types policy: a deny-list of the elements it never builds | installation/WebAdmin/index.html:531-539 |
| The Deck escapes by hand: `escHtml(` 912 times; translations may not hold `< > & "` | index.html; `sanitiseCatalogue` index.html:637-643; `check-deck-languages.py` |
| The page and its policy held to each other | `build/check-deck-script.py`, `build/trusted-types-test.js` |
| The webmail: `script-src 'self'` (only the server's fixed routes), `trusted-types default`, `frame-src 'self' blob:`, `worker-src 'self'`, `form-action 'none'` | RestApiPortal.cpp:50-55 |
| The webmail's `default` policy: DOMParser input unchanged, page markup stripped of never-built elements, exactly two script addresses | Portal.js:26-65 |
| The webmail builds its screens with DOM calls (3 `innerHTML`) | Portal.js; `build/check-portal-script.py` |
| A message's HTML: its own document, `default-src 'none'`, no script, `sandbox allow-popups allow-popups-to-escape-sandbox`, `form-action 'none'`, remote images only when asked; the policy in the header and in a meta tag | RestApiServer.cpp:18560-18587; the frame Portal.html:1189 |
| Attachments: HTML, SVG, XML and script as `application/octet-stream`; `Content-Disposition: attachment`; `Content-Security-Policy: sandbox`; nosniff | RestApiServer.cpp:20011-20015, and the type rule above it |
| Files sent as links: only types no browser runs keep their type; sandbox policy; the password form's page under `default-src 'none'` and `X-Frame-Options: DENY` | RestApiFiles.cpp:408-412, :815-817 |
| The script-free pages - seven, from six builders, since `/recovery/verify` and `/portal/reset` share one: no script at all | RestApiAccountRecovery.cpp:141, RestApiConfidential.cpp:206, RestApiMailingLists.cpp:181, RestApiQuarantineDigest.cpp:126, RestApiMigrations.cpp:365, RestApiFiles.cpp:408 |
| The other downloads under `Content-Security-Policy: sandbox` | RestApiAccountResources.cpp:1329, RestApiContacts.cpp:545, RestApiDirectory.cpp:464, RestApiDiscovery.cpp:953, RestApiOperator.cpp:292, RestApiScheduled.cpp:385, RestApiInboundWebhooks.cpp:712 |

### The service worker and the browser's storage

| Control | Where |
|---|---|
| The worker is a fixed route compiled in; `worker-src 'self'`; a worker needs a script type, which no attachment or shared file is served as | RestApiServer.cpp:2723, :21034-21042 |
| A fixed shell list, network first | PortalServiceWorker.js:6-14, :185-189 |
| Offline parts answered from IndexedDB with `Content-Security-Policy: sandbox` and attachment disposition for the types a browser would run | PortalServiceWorker.js:84-160 |
| Web Push: the payload encrypted to the subscription; the worker shows it and fetches nothing | PortalServiceWorker.js:16-76 |

### Keys in the browser (A-6)

| Control | Where |
|---|---|
| The server never holds an OpenPGP private key in the clear; a secret-key packet in an upload is refused | docs/OpenPGP.md:17, :235-236 |
| The secret half stored encrypted under the page's passphrase, which the server never sees; unlocked for the page session only | docs/OpenPGP.md:404-436 |
| The encrypted secret keys withheld from app passwords and from an administrator's support session (each is an offline test of a passphrase guess) | docs/OpenPGP.md:847 |
| Zero-access mailboxes: the keys wrapped under the account password or a recovery key; re-wrapping refused where the page never saw the old password | docs/ZeroAccess.md |

The surface after Programme A
-----------------------------

What changes, and what it brings:

| Change | Effect on the threats |
|---|---|
| The Deck's views built with Preact; no `innerHTML`; `trusted-types 'none'` | T-2: escaping by construction, and every HTML sink a hard failure in the browser rather than a string a deny-list filters |
| The webmail's one named policy for `DOMParser` only | T-2: a raw-HTML path anywhere else in the page fails closed |
| `style-src` by hash (if the A2 spike shows it can be) | T-2: no CSS injection through a style attribute |
| The Deck compiled into the binary | T-7: a changed file on disk changes nothing (W-4) |
| Hashed file names, a cache-first worker scoped to `/portal` with today's root registration taken away, the cache named from the build hash | T-6: the worker no longer controls the Deck (W-5); a stale or mixed shell cannot survive an update |
| The npm supply chain (T-10) | New. Held by: one runtime package with no dependencies; exact pins; a lock checked for registry, integrity and package count; `--ignore-scripts`; a provenance attestation required of every package but TypeScript's; a 14-day release age; the bundle committed unminified and reviewed as a diff; byte-identical rebuilds in three environments; nothing fetched from GitHub; an SBOM. **TypeScript is the unattested part of the build**: `typescript` 7.0.2 and its native `@typescript/typescript-linux-x64` carry no provenance (measured on server2, 13 October 2026) and are two thirds of the lock's download. It is kept out of the shipped bytes' path by the build, not by its role: the bundle is built from an install that omits it (`npm ci --omit=dev`, TypeScript the only development dependency: three packages, all attested), and `tsc --noEmit` runs in a job of its own over a read-only checkout, so that nothing it runs can write the sources, `node_modules` or the bundle |
| A CSP report endpoint | A new anonymous route: held to a body cap, a per-address budget, two content types, nothing echoed and nothing stored (A4) |
| ZAP, fuzzing, real-browser tests under the served headers | Verification that runs on every pipeline or schedule, not once |

Findings
--------

Each finding was found by the matrix's flags or by reading the code for this
model, and was then checked by reading the handler. None was run against a
server. Severity is for this server's typical deployment (the REST listener on
loopback behind a proxy, or on TLS).

The numbers W-1 to W-3 and W-10 to W-12 name controls of the server, each
described above where it applies - the refusals a credential confined to named
domains meets (W-1, W-3, W-10), the refusal of a credentialed change sent from
another site (W-2), the ACME responder's connections (W-11) and the two
anonymous posts (W-12) - and the code and its tests name them by these numbers.
The findings that follow, W-4 to W-9, are what the rebuild answers.

### W-4 (Low): the Deck's integrity is the program folder's

The Deck is read from `<ProgramFolder>/WebAdmin/index.html` on every request and
its script hash is computed from the bytes read (RestApiServer.cpp:11618-11675).
The policy keeps injected script out of the page; it does nothing about a changed
file, which is then the administrator's console with a matching hash. The
catalogues beside it are read the same way, held only by the ban on `< > & "`.
The protection is the program folder's permissions, as it is for the programs
beside it.

**Fix:** compile the Deck and its catalogues into the binary, as the webmail is
(A2, end; ControlDeckFramework.md).

### W-5 (Low): the webmail's service worker controls the whole origin

`/portal-sw.js` is registered without a scope (Portal.js:7826), so its scope is
its directory, `/`, which `Service-Worker-Allowed: /` also allows
(RestApiServer.cpp:21034-21042). In a browser that has used the webmail, the
worker sees every fetch the Deck makes. Today it passes them through untouched
(PortalServiceWorker.js:180-189), so nothing is exposed; but the Deck's security
then depends on the webmail's worker never changing that.

**Fix (A3):** registering again with `scope: '/portal'` is not enough on its own.
Registrations are kept by scope, so every browser that has used the webmail
keeps today's registration of `/` beside the new one, and that one goes on
controlling the Deck and updating, its script still being `/portal-sw.js`; and
`Service-Worker-Allowed: /` grants nothing that a script at the root does not
have already. Neither Portal.js nor PortalServiceWorker.js unregisters anything
today. So the page asks for `navigator.serviceWorker.getRegistration('/')` and
unregisters it when its scope is the origin's root, and the worker unregisters
itself when `self.registration.scope` is the root; then the page registers with
`scope: '/portal'`, and the header goes. That code stays for at least one
release, and a browser test shows a page under `/` uncontrolled after an
upgrade from today's worker.

### W-6 (Low): any site can fill in a message to be sent

The share target is answered by the worker (`POST /portal/share`,
PortalServiceWorker.js:162-178), which keeps the posted title, text, link and
files and opens a new message with them. Any page can post that form. The
person still has to send it, and sees what it says, so this is a nuisance and a
social-engineering aid rather than a forgery.

**Fix:** in A3, mark a shared message as coming from outside until the person
edits it, or accept the post only when the worker can see it came from the
operating system's share sheet (no `Referer`).

### W-7 (Low): style attributes and the deny-list policies

Both pages allow `style-src 'unsafe-inline'` (RestApiServer.cpp:11675;
RestApiPortal.cpp:50), and both make a `default` Trusted Types policy that
passes any markup not on a deny-list (index.html:532-538; Portal.js:26-65). An
HTML injection that survives `escHtml` therefore still lands in the page with
style attributes and ordinary elements (measured in the evaluation: Preact's raw
escape, refused under `trusted-types 'none'`, is let through by the Deck's
`default` policy). The Deck's protection rests on 912 hand-written `escHtml`
calls.

**Fix:** the rebuild's endgame: no `innerHTML`, `trusted-types 'none'` for the
Deck, one named policy for the webmail, styles by hash.

### W-8 (Low): the PDF preview frame has no sandbox

The webmail shows a PDF attachment in a frame without a `sandbox` attribute, from
a `blob:` address of the page's own origin (Portal.js:3176-3190), because
Chromium refuses PDFs in any sandboxed frame and Firefox draws nothing in one
without script. The type is forced to `application/pdf`, so the bytes go to the
browser's own viewer, which runs apart from the page. Accepted, and written down
here so that the rebuild keeps the forced type.

### W-9 (Informational): smaller items

- **JSON answers carry no `X-Content-Type-Options: nosniff`** (`BuildResponse_`,
  RestApiServer.cpp:10628-10646 adds `Cache-Control` only). No current browser
  renders `application/json` as a page; add the header for completeness.
- **A stale comment.** RestApiServer.cpp:2497 says the Deck's page "is the only
  unauthenticated route in the listener"; 41 routes come before
  `Authenticate_`. The matrix lists them; the comment should say so.
- **A domain administrator reads the custom role definitions**
  (`GET /api/v1/roles`, `view_configuration`): names and permission sets, no
  holders outside its domain (the assignment list is filtered,
  RestApiRoles.cpp:230-234). Acceptable.

### The routes the matrix flags, judged

The matrix flags every route that a role confined to its domain reaches although
the route names no domain, because there the handler, not `Authorize_`, decides
what the role sees; and every change such a role makes on its own domain's path
through a handler that is not given the caller, because there nothing holds what
the body names beyond that domain to the credential.

| Route | Who reaches it confined | Judgement |
|---|---|---|
| `Status` | domain admin, one-domain key | Intended (comments at RestApiServer.cpp:10297-10303 and :10605-10612): its version and state are what the Deck needs; its counters - messages processed, spam detected, viruses removed and sessions open (RestApiServer.cpp:11730-11739) - are the whole server's, and name no domain, account or address |
| `RoleList`, `RoleGet` | domain admin, one-domain key | W-9, acceptable |
| `Tlsa` | domain admin, one-domain key | Intended: the server's own TLSA records |
| `OpenApi` | domain admin, help desk, one-domain key | The API's description; nothing of the server's |
| `BounceClassify` | domain admin, one-domain key | Classifies the text in the body; reads nothing |
| `PortalFilesPolicy` (GET) | domain admin, one-domain key | The server-wide file-link policy; a setting, no data. Acceptable |
| `Srv`, `DomainList` | domain admin, help desk, one-domain key | The handler filters to the credential's domains (verified) |
| `RoleAssignmentList` | domain admin, one-domain key | Filtered to the credential's domains (RestApiRoles.cpp:230-234, verified) |
| `Migration*` | domain admin, one-domain key | Confined by the jobs' target domains; a confined credential cannot create one (RestApiMigrations.cpp:512-568, verified) |
| `PortalBrandingPut`, `PortalFilesPolicyPut` | one-domain key | The handlers confine to the credential's domains (`DomainInBodyRefused_`: RestApiBranding.cpp:141-162, RestApiFiles.cpp:839-858, verified) |
| `Archive*`, `Discovery*`, `InboundWebhook*`, `Scim*` | one-domain key | The handlers are given the credential's domains (`CaseVisible`, `scim.domains`); not re-verified line by line for this model, but for the inbound webhooks' delivery rule (Entry points, "Outbound requests") |
| `F-write-blind`: `AliasCreate`, `AliasDelete`, `DomainAliasDelete`, `ListDelete`, `ListMemberApprove`, `ListMemberResetBounces`, `SuppressionCreate`, `SuppressionDelete`, `FetchAccountDelete`, `FetchAccountDownload`, `AccountOpenPgpDelete`, `AccountAddressBookShareDelete` | domain admin, one-domain key | Each acts inside what the path names: the alias's name is held to the path's domain (RestApiRoutes.cpp:1185-1189); a domain alias, a fetch account, a key and a share are looked up in the path's domain's or account's own collection (RestApiAdministration.cpp:1165-1171, RestApiFetchAccounts.cpp:754-760, :778-784, RestApiOpenPgp.cpp:282-290, RestApiAddressBooks.cpp:690-709); a list's member in the path's list (RestApiMailingLists.cpp:582-608); a suppression in the path's domain's list (verified by this model's review) |
| `F-anonymous-get`: the ten pages answered before any credential for GET and POST alike | anonymous | Each acts on POST; its GET shows the page or its form (RestApiFiles.cpp:772-787, RestApiConfidential.cpp:385-398, RestApiAccountRecovery.cpp:648 and :753, RestApiQuarantineDigest.cpp:168 and :207, RestApiMailingLists.cpp:745, :789, :872 and :913, all verified). Two record something on GET, by design: a file link without a password serves the file and counts the download (RestApiFiles.cpp:798), and a confidential message records that it was opened only for the grant cookie its own POST set or for the recipient's own webmail session |

A change on a domain's path whose handler is given the caller is not flagged,
since the handler can hold the body to the credential: a domain's update, which
holds its name and its relay host (W-10), and a domain's or an account's rules,
whose handlers are given whether the caller holds the server's authority and
hold the three actions to it (W-10).

How this stays true
-------------------

- `build/generate-auth-matrix.py --check` runs in the `checkers` job: a route
  added, moved or reclassified changes the committed matrix, and a new route of
  a flagged shape is flagged the day it is written. The steps of the functions
  the matrix's answer is read from - `Authorize_`, the functions whose lists it
  reads, the four built-in roles' functions, the `Bit` they set a permission
  with and the `RoleManager::BuiltInPermissions_` that picks one by a role's
  name, `RoleManager::Allows` and the `RoleGrant::Holds` and
  `RoleManager::IsDomainReached_` it asks, `IsDomainAllowed_`,
  `IsServerWideReportSection_` and the `FindSection` it asks, and
  `ProcessRequest_` from the end of the anonymous routes to the handler - are
  held to the ones the script follows: one added, taken out, moved or given a
  condition, or a name one compares or is kept under changed, stops the script
  until it has been read against it. So does a definition it reads that stands
  inside a conditional of the preprocessor, which it does not evaluate, or
  holds one, or is not the only definition of its name in its file (an
  overload, a copy in another namespace), a directive of the preprocessor of
  any other kind inside what it reads (an `#include`, which brings in code it
  never sees), and an enumerator of the route kinds or of the permissions
  whose value, as the compiler gives it, is not its place - a permission's
  place is the bit it is set at. It reads the code as the compiler's first
  three translation phases do: what a comment or a literal holds is not read,
  a directive is one wherever it begins a line, after spaces, a comment or a
  line splice too, and a form it does not read as the compilers do, such as a
  backslash with spaces after it at a line's end, stops it. What
  `Authenticate_` makes of a credential - the grant of a role, for its
  domains - the string helpers, a macro defined elsewhere over a name those
  functions use, an overload of one of them defined in another file and the
  functions `ProcessRequest_`'s other checks call are not held; the matrix's
  "Not shown" names those functions. Its `--self-test` breaks the source in
  memory and proves each flag still fires and each such change stops it.
- `check-role-choke-point.py`, `check-authz-choke-point.py`,
  `check-deck-script.py` and `check-portal-script.py` hold the choke points and
  the pages' policies.
- This document is revised at the end of each Programme A batch, and the
  independent review (A5) starts from it.
