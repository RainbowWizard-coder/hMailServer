// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Passkeys over the REST API (WebAuthn.h, PasskeyStore.h):
//
//    GET    /api/v1/passkeys                 the signed-in credential's own
//    POST   /api/v1/passkeys/options         PublicKeyCredentialCreationOptions
//    POST   /api/v1/passkeys                 the credential the browser made
//    DELETE /api/v1/passkeys/{id}            remove one
//    POST   /api/v1/passkeys/challenge       the sign-in challenge; NO credential
//
// "The signed-in credential's own" is the point of this file's shape: the same
// four routes answer an account's session with that account's passkeys and the
// administrator's session with the administrator's, because a passkey belongs
// to whoever signed in and not to a resource somebody administers. They are
// classified in RestApiServer::IsSessionRoute_ with the three session routes
// for the same reason - they are about the credential rather than about the
// server - and an API key is refused them all.
//
// Registering one asks for the owner's password again (and the one-time code
// where a second factor is enrolled), exactly as minting an app password does
// and for the same reason: a stolen session must not be able to add a way in
// that outlives it. A request that was itself authenticated with an app
// password is refused outright.
//
// THE SIGN-IN itself is not here. A passkey assertion arrives at
// POST /api/v1/session in place of a password, is verified in
// RestApiServer::AuthenticatePasskey_ below (which Authenticate_ calls before
// anything else looks at the request) and mints exactly the session a password
// would. So there is one place a browser session is made, and one place the
// policy that governs it is applied.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "PasskeyStore.h"
#include "SignInBudget.h"
#include "Crypt.h"
#include "Totp.h"
#include "Unicode.h"
#include "WebAuthn.h"
#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Logger.h"
#include "../Application/RoleManager.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"

#include <ctime>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int ChallengeMilliseconds = PasskeyStore::ChallengeSeconds * 1000;

      // The cookie a sign-in challenge is handed out with. Not a credential:
      // it names a challenge this server is holding and nothing else.
      const char *PasskeyCookieName = "hmpasskey";

      std::string Bytes(const AnsiString &text)
      {
         return std::string(text.c_str(), (size_t) text.GetLength());
      }

      // The base64url the browser writes for a binary field, decoded.
      bool Binary(const JsonValue *object, const char *key, std::string &bytes)
      {
         if (object == nullptr)
            return false;
         const std::string text = object->GetString(key);
         return !text.empty() && text.size() <= 8192 && WebAuthn::Base64UrlDecode(text, bytes) && !bytes.empty();
      }

      AnsiString PasskeyJson(const PasskeyStore::Passkey &passkey)
      {
         AnsiString numbers;
         numbers.Format("{\"id\":%I64d,\"created\":%I64d,\"last_used\":%I64d,\"algorithm\":%d,",
            passkey.id, passkey.created, passkey.last_used, passkey.algorithm);
         return numbers +
            "\"name\":\"" + RestApiServer::JsonEscapeUtf8(passkey.name) + "\"," +
            "\"transports\":\"" + RestApiServer::JsonEscapeUtf8(passkey.transports) + "\"," +
            "\"aaguid\":\"" + RestApiServer::JsonEscapeUtf8(String(passkey.aaguid)) + "\"}";
      }

      // "https://mail.example.test:8443" -> host "mail.example.test:8443".
      // Empty for anything that is not an absolute http or https origin.
      AnsiString HostOfOrigin(const std::string &origin)
      {
         AnsiString text(origin.c_str());
         AnsiString scheme;
         if (text.StartsWith("https://"))
         {
            scheme = "https";
            text = text.Mid(8);
         }
         else if (text.StartsWith("http://"))
         {
            scheme = "http";
            text = text.Mid(7);
         }
         else
            return "";

         if (text.Find("/") >= 0 || text.IsEmpty())
            return "";

         // A browser treats http as a secure context only on a loopback name,
         // so an assertion collected at plain http anywhere else did not come
         // from a page a browser would have run this on.
         if (scheme == "http")
         {
            AnsiString host = text;
            const int colon = host.Find(":");
            if (colon >= 0)
               host = host.Mid(0, colon);
            host.MakeLower();
            if (host != "localhost" && host != "127.0.0.1" && host != "[::1]")
               return "";
         }

         text.MakeLower();
         return text;
      }
   }

   AnsiString
   RestApiServer::OpenApiPasskeyPaths_()
   {
      static const char *paths =
         ",\"/api/v1/passkeys\":{"
         "\"get\":{\"summary\":\"The passkeys the signed-in credential holds\",\"description\":\"{enabled,rp_id,passkeys:[{id,name,created,last_used,transports,algorithm,aaguid}]}: the account's own for an account's session, the administrator's own for the administrator's. created and last_used are seconds since 1970; last_used is 0 for one never used. The public key is never emitted. enabled is the PasskeysEnabled setting and rp_id is what a credential made now would be bound to.\",\"responses\":{\"200\":{\"description\":\"The passkeys\"}}},"
         "\"post\":{\"summary\":\"Register the credential the browser just made\",\"description\":\"Body: name (what the device is called), password (the owner's own password, as making an app password asks for it; the one-time code goes in X-hMailServer-OTP where a second factor is enrolled) and credential, the PublicKeyCredential the browser answered with, its binary fields base64url: {id, response:{clientDataJSON, attestationObject, transports:[..]}}. The client data's type, challenge and origin are checked, the attestation object is CBOR-decoded for its authenticator data, the relying party hash and the user-present and user-verified flags are checked, and the COSE public key is stored - ES256 or RS256 and nothing else. The attestation STATEMENT is not verified and this server holds no metadata to verify it against: see docs/PasskeysAndSingleSignOn.md.\",\"responses\":{\"201\":{\"description\":\"The passkey\"},\"400\":{\"description\":\"The credential did not answer the challenge this server issued\"},\"403\":{\"description\":\"The password was wrong, or the request came in on an app password\"},\"409\":{\"description\":\"The administrator's second factor is enrolled but its secret cannot be opened on this machine\"}}}},"
         "\"/api/v1/passkeys/options\":{\"post\":{\"summary\":\"What the browser needs to make a passkey\",\"description\":\"PublicKeyCredentialCreationOptions as navigator.credentials.create takes them, the binary fields base64url: challenge, rp, user, pubKeyCredParams (ES256 and RS256), timeout, attestation 'none', authenticatorSelection (residentKey preferred, userVerification required - the registration refuses a credential the authenticator did not verify the person for, and so does every assertion afterwards) and excludeCredentials, which is this owner's credentials so the same authenticator is not enrolled twice. The challenge is kept against this session for five minutes and is spent by the POST that follows.\",\"responses\":{\"200\":{\"description\":\"The options\"},\"403\":{\"description\":\"Passkeys are off on this server\"}}}},"
         "\"/api/v1/passkeys/{id}\":{\"delete\":{\"summary\":\"Remove one of the signed-in credential's passkeys\",\"description\":\"The device is not told; it keeps a private key this server no longer knows. Refused on a request authenticated with an app password. Answered while PasskeysEnabled is off as well, because taking a way in away must not depend on the feature being switched on.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"Not one of yours\"}}}},"
         "\"/api/v1/passkeys/challenge\":{\"post\":{\"summary\":\"The challenge a passkey sign-in answers\",\"description\":\"No credential: there is no session yet. {challenge, rp_id, timeout} and a Set-Cookie (hmpasskey, HttpOnly, five minutes) the sign-in must carry back, which is what binds the challenge to this browser. allowCredentials is deliberately empty, so a discoverable credential can name its own account. The cookie holds the challenge and its end, sealed (AES-256-GCM, a key of the server process's): nothing is kept on the server, so no number of challenges asked for can push anybody's out. The challenge is spent by the first POST /api/v1/session whose assertion over it verifies, so an assertion cannot be replayed.\",\"responses\":{\"200\":{\"description\":\"The challenge\"},\"403\":{\"description\":\"Passkeys are off on this server\"}}}},"
         "\"/api/v1/signin-methods\":{\"get\":{\"summary\":\"What the sign-in pages may offer\",\"description\":\"No credential. {passkeys, oidc, oidc_label, saml, saml_label}: whether a passkey button, a 'Sign in with ...' button for the OpenID Connect issuer and one for the SAML identity provider should be drawn, the issuer's host for the second, and for the third the words saml_button_label gives it (Sign in with SAML by default). A page that asks this learns nothing a stranger could not learn by pressing the buttons.\",\"responses\":{\"200\":{\"description\":\"The methods\"}}}}";
      return AnsiString(paths);
   }

   // Who the request is acting for, which is the whole of the ownership
   // decision for every route in this file.
   bool
   RestApiServer::PasskeyOwner_(const Caller &caller, __int64 &accountId, bool &administrator, String &owner)
   {
      administrator = caller.result == AuthenticatedAsAdministrator;
      accountId = administrator ? 0 : (caller.account ? caller.account->GetID() : 0);
      owner = administrator ? String(_T("the administrator")) : (caller.account ? caller.account->GetAddress() : String());
      return administrator || accountId > 0;
   }

   // The key a registration challenge is held against: the session, where there
   // is one, and otherwise the credential and the address it came from. A
   // challenge is then answerable only by the client that asked for it.
   AnsiString
   RestApiServer::PasskeyChallengeKey_(const Caller &caller)
   {
      if (!caller.session_hash.IsEmpty())
         return "s:" + caller.session_hash;
      return "c:" + caller.identity + "@" + caller.peer.ToString();
   }

   // The administrator's own password, proved again, as ConfirmAccountPassword_
   // proves an account's. Same rule, same second factor, same reason.
   bool
   RestApiServer::ConfirmAdministratorPassword_(const Caller &caller, const AnsiString &request, HttpResponse &failure)
   {
      const String password = JsonUtf8Value_(GetRequestBody_(request), "password");
      if (password.IsEmpty())
      {
         failure = BuildResponse_(400, "{\"error\":\"password is required: the administrator password, to prove it is the administrator asking\"}");
         return false;
      }

      // The administrator password's failed-sign-in throttle (ProcessRequest_)
      // holds here too, since 6.3.6: this is a guess of the same password from a
      // session that did not need it - an administrator passkey, a single
      // sign-on, a session cookie - and the throttle there reads only an
      // Authorization: Basic header. A guess inside the delay is refused before
      // the password is looked at, and a wrong one lengthens the delay.
      const int waitMs = SessionThrottleRetryMs_("administrator", caller.peer);
      if (waitMs > 0)
      {
         const int retryAfter = (waitMs + 999) / 1000;
         AnsiString headers;
         headers.Format("Retry-After: %d\r\n", retryAfter < 1 ? 1 : retryAfter);
         failure = BuildResponse_(429, "{\"error\":\"too many sign-in attempts; wait and try again\"}", headers);
         return false;
      }

      const String correct = IniFileSettings::Instance()->GetAdministratorPassword();
      if (correct.IsEmpty() ||
          !Crypt::Instance()->Validate(password, correct, Crypt::Instance()->GetHashType(correct)))
      {
         LOG_APPLICATION("REST API: registering an administrator passkey was refused from " +
            String(caller.peer.ToString()) + " - the password did not match.");
         RegisterAuthenticationFailure_(caller.peer);
         RegisterSessionFailure_("administrator", caller.peer);
         failure = BuildResponse_(403, "{\"error\":\"the password is not correct\"}");
         return false;
      }

      ClearSessionFailure_("administrator", caller.peer);

      const String secret = IniFileSettings::Instance()->GetAdministratorTotpSecret();

      // Enrolled but unopenable: refused, as the administrator's sign-ins
      // refuse it (AuthenticateBasic_, COMAuthentication) - a lost key must not
      // read as a second factor switched off, here least of all, where what is
      // being added is a way in that needs no code.
      if (IniFileSettings::Instance()->IsAdministratorTotpEnrolled() && secret.IsEmpty())
      {
         LOG_APPLICATION("REST API: registering an administrator passkey was refused: a second factor is enrolled but its secret cannot be opened on this machine.");
         failure = BuildResponse_(409, "{\"error\":\"the administrator's second factor is enrolled but its secret cannot be opened on this machine, so it cannot be checked; restore the key it was protected with, or clear AdministratorTotpSecret in hMailServer.ini and enrol again\"}");
         return false;
      }

      if (!secret.IsEmpty())
      {
         // As SecondFactorProved_ does for an account: no code is a page that
         // has not been told one is needed, and a wrong one is a guess, counted
         // against the administrator's own wrong-code budget and the source
         // address - and the budget asked first, so a spent one refuses the
         // right code too.
         const AnsiString code = GetHeader_(request, "x-hmailserver-otp");
         if (code.IsEmpty())
         {
            failure = BuildUnauthorizedResponse_(true, IsPageScriptRequest_(request));
            return false;
         }

         // Taken once, as every code is (SecondFactorGuard): one the
         // administrator has had accepted already, here or at a sign-in, is
         // refused and not counted. The budget is asked, the code judged and a
         // wrong one counted under one lock (TakeCode).
         const String owner = _T("administrator");
         SecondFactorGuard::Outcome outcome;
         const SecondFactorGuard::Taken taken = SecondFactorGuard::TakeCode(owner, AnsiString(secret), code, &outcome);
         if (taken == SecondFactorGuard::CodeLocked)
         {
            LOG_APPLICATION("REST API: a one-time code for the administrator from " + String(caller.peer.ToString()) +
               " was not looked at: too many wrong codes have been tried for it.");
            failure = TooManyWrongCodes_(outcome.retry_after);
            return false;
         }

         if (taken != SecondFactorGuard::CodeTaken)
         {
            if (taken == SecondFactorGuard::CodeReused)
               SecondFactorReused_(owner, caller.peer);
            else
               SecondFactorWrong_(owner, false, caller.peer, outcome.wrong, outcome.locked_now);
            failure = BuildUnauthorizedResponse_(true, IsPageScriptRequest_(request));
            return false;
         }

         SecondFactorRight_(owner);
      }

      return true;
   }

   HttpResponse
   RestApiServer::HandlePasskeys_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &request)
   {
      __int64 accountId = 0;
      bool administrator = false;
      String owner;
      if (!PasskeyOwner_(caller, accountId, administrator, owner))
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const AnsiString host = GetHeader_(request, "host");
      const AnsiString relyingParty = PasskeyStore::RelyingParty(host);

      if (kind == RoutePasskeyList)
      {
         std::vector<PasskeyStore::Passkey> passkeys;
         if (!PasskeyStore::List(accountId, administrator, passkeys))
            return BuildResponse_(500, "{\"error\":\"the passkeys could not be read\"}");

         AnsiString body = AnsiString("{\"enabled\":") + (PasskeyStore::Enabled() ? "true" : "false") +
            ",\"rp_id\":\"" + JsonEscape_(relyingParty) + "\",\"passkeys\":[";
         for (size_t i = 0; i < passkeys.size(); i++)
            body += (i ? "," : "") + PasskeyJson(passkeys[i]);
         return BuildResponse_(200, body + "]}");
      }

      // Listing and removing are above the switch, and deliberately: taking a
      // way in away must never depend on the feature being switched on. An
      // administrator who turns passkeys off because a device was lost is at
      // the very moment its owner most wants that device's credential gone,
      // and the setting's own description says the enrolled passkeys stay in
      // the database "until they are removed". The two routes the switch has
      // to guard are the two that would add one.
      if (kind == RoutePasskeyDelete)
      {
         HttpResponse refusal;
         if (RefuseAppPasswordCaller_(caller, "removing a passkey", refusal))
            return refusal;

         if (!PasskeyStore::Remove(accountId, administrator, id))
            return BuildResponse_(404, "{\"error\":\"no such passkey\"}");

         LOG_APPLICATION("REST API: a passkey of " + owner + " was removed from " + String(caller.peer.ToString()) + ".");
         return BuildResponse_(200, "{\"deleted\":true}");
      }

      if (!PasskeyStore::Enabled())
         return BuildResponse_(403, "{\"error\":\"passkeys are switched off on this server\"}");

      if (relyingParty.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"this server cannot work out a relying party id for the host this page was served on; set passkey_relying_party\"}");

      const AnsiString missingOrigins = PasskeyStore::MissingOriginsProblem();
      if (!missingOrigins.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(missingOrigins) + "\"}");

      if (kind == RoutePasskeyOptions)
      {
         AnsiString challenge;
         if (!PasskeyStore::OfferRegistration(std::string(caller.identity.c_str()), PasskeyChallengeKey_(caller), challenge))
            return BuildResponse_(503, "{\"error\":\"no challenge could be made; try again\"}");

         std::vector<PasskeyStore::Passkey> passkeys;
         PasskeyStore::List(accountId, administrator, passkeys);

         AnsiString exclude;
         for (size_t i = 0; i < passkeys.size(); i++)
            exclude += AnsiString(i ? "," : "") + "{\"type\":\"public-key\",\"id\":\"" + JsonEscape_(passkeys[i].credential) + "\"}";

         const AnsiString name = administrator ? AnsiString("Administrator") : Utf8_(owner);
         AnsiString body;
         body.Format("{\"challenge\":\"%hs\",\"timeout\":%d,\"attestation\":\"none\","
                     "\"rp\":{\"id\":\"%hs\",\"name\":\"hMailServer\"},"
                     "\"user\":{\"id\":\"%hs\",\"name\":\"%hs\",\"displayName\":\"%hs\"},"
                     "\"pubKeyCredParams\":[{\"type\":\"public-key\",\"alg\":-7},{\"type\":\"public-key\",\"alg\":-257}],"
                     // userVerification required, because the registration
                     // refuses a credential the authenticator did not verify
                     // the person for and so does every assertion afterwards:
                     // asking for what is demanded makes the device ask for
                     // the PIN, where "preferred" would let it make a
                     // credential this server then refuses to store.
                     "\"authenticatorSelection\":{\"residentKey\":\"preferred\",\"userVerification\":\"required\"},"
                     "\"excludeCredentials\":[%hs]}",
            challenge.c_str(), ChallengeMilliseconds,
            JsonEscape_(relyingParty).c_str(),
            PasskeyStore::UserHandle(accountId, administrator).c_str(),
            JsonEscape_(name).c_str(), JsonEscape_(name).c_str(),
            exclude.c_str());

         return BuildResponse_(200, body);
      }

      if (kind != RoutePasskeyCreate)
         return BuildResponse_(404, "{\"error\":\"not found\"}");

      // The owner's own password, proved again. An app-password request is
      // refused by ConfirmAccountPassword_ before the password is even read.
      // A recovery code that stood in for the one-time code is matched here
      // and spent below, once the passkey is stored: a registration refused
      // after this point - a credential the authenticator did not verify the
      // person for, a challenge already spent - must not cost the account one
      // of its ten. Without the id the helper would match the code and nobody
      // would spend it, and the same code would register a passkey for ever.
      HttpResponse refusal;
      __int64 recoveryCodeId = 0;
      if (administrator)
      {
         if (!ConfirmAdministratorPassword_(caller, request, refusal))
            return refusal;
      }
      else if (!ConfirmAccountPassword_(caller, request, refusal, "registering a passkey", &recoveryCodeId))
         return refusal;

      const AnsiString requestBody = GetRequestBody_(request);
      JsonValue body;
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      String name = JsonUtf8Value_(requestBody, "name");
      name.TrimLeft();
      name.TrimRight();
      if (name.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name is required: what the device is called\"}");
      if (name.GetLength() > 255)
         return BuildResponse_(400, "{\"error\":\"name is at most 255 characters\"}");

      const JsonValue *credential = body.Get("credential");
      const JsonValue *response = credential == nullptr ? nullptr : credential->Get("response");
      std::string clientDataJson, attestationObject;
      if (!Binary(response, "clientDataJSON", clientDataJson) || !Binary(response, "attestationObject", attestationObject))
         return BuildResponse_(400, "{\"error\":\"credential.response must carry clientDataJSON and attestationObject as base64url\"}");

      AnsiString challenge;
      if (!PasskeyStore::TakeRegistration(PasskeyChallengeKey_(caller), challenge))
         return BuildResponse_(400, "{\"error\":\"there is no challenge waiting for this session, or it has expired; start again\"}");

      AnsiString problem;
      WebAuthn::ClientData clientData;
      if (!WebAuthn::ReadClientData(clientDataJson, clientData, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      if (clientData.type != "webauthn.create")
         return BuildResponse_(400, "{\"error\":\"the client data is not a registration\"}");
      if (AnsiString(clientData.challenge.c_str()) != challenge)
         return BuildResponse_(400, "{\"error\":\"the credential answers a different challenge\"}");

      // The origin the browser says the page was, held to this server's own
      // (PasskeyStore::OriginAllowed): the page's host by default, the named
      // list once a relying party is set - never merely whatever the Host
      // header of this request says. And not a frame of another site's page.
      AnsiString originProblem;
      if (clientData.cross_origin ||
          !PasskeyStore::OriginAllowed(AnsiString(clientData.origin.c_str()), HostOfOrigin(clientData.origin), host, originProblem))
      {
         LOG_APPLICATION("REST API: a passkey registration for " + owner + " from " + String(caller.peer.ToString()) +
            " was refused: " + String(clientData.cross_origin ? AnsiString("it was made in a frame of another site's page") : originProblem) + ".");
         return BuildResponse_(400, "{\"error\":\"the credential was made at another origin\"}");
      }

      std::string authenticatorData;
      AnsiString format;
      if (!WebAuthn::ReadAttestationObject(attestationObject, authenticatorData, format, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      WebAuthn::AuthenticatorData authData;
      if (!WebAuthn::ReadAuthenticatorData(authenticatorData, authData, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      if (authData.rp_id_hash != WebAuthn::Sha256(Bytes(relyingParty)))
         return BuildResponse_(400, "{\"error\":\"the credential is bound to another relying party\"}");
      if ((authData.flags & WebAuthn::FlagUserPresent) == 0)
         return BuildResponse_(400, "{\"error\":\"the authenticator did not say the person was present\"}");
      if ((authData.flags & WebAuthn::FlagUserVerified) == 0)
         return BuildResponse_(400, "{\"error\":\"the authenticator did not verify the person, so this passkey could not stand in for the one-time code; set it up to ask for a PIN or a fingerprint\"}");
      if (!authData.attested)
         return BuildResponse_(400, "{\"error\":\"the registration carries no credential\"}");

      WebAuthn::PublicKey key;
      if (!WebAuthn::ReadPublicKey(authData.public_key, key, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      PasskeyStore::Passkey passkey;
      passkey.account_id = accountId;
      passkey.administrator = administrator;
      passkey.credential = WebAuthn::Base64Url(authData.credential_id);
      passkey.public_key = WebAuthn::Base64Url(authData.public_key);
      passkey.algorithm = key.algorithm;
      passkey.sign_count = authData.sign_count;
      passkey.name = name;
      passkey.aaguid = WebAuthn::Base64Url(authData.aaguid);
      passkey.created = (__int64) time(nullptr);
      passkey.last_used = 0;

      // What the browser said about how the authenticator is reached. A hint
      // for the next sign-in and for the list; nothing is decided from it, so
      // it is taken as it comes and clipped.
      const JsonValue *transports = response->Get("transports");
      String transportWords;
      for (size_t i = 0; transports != nullptr && transports->IsArray() && i < transports->Size(); i++)
      {
         const JsonValue *one = transports->At(i);
         if (one == nullptr || !one->IsString() || one->AsString().size() > 16)
            continue;
         String word;
         Unicode::MultiByteToWide(AnsiString(one->AsString().c_str()), word);
         if (!transportWords.IsEmpty())
            transportWords += _T(",");
         transportWords += word;
      }
      passkey.transports = transportWords.GetLength() > 255 ? transportWords.Mid(0, 255) : transportWords;

      String failure;
      if (!PasskeyStore::Add(passkey, failure))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(failure)) + "\"}");

      if (!administrator)
         SpendSecondFactor_(caller.account, recoveryCodeId);

      LOG_APPLICATION("REST API: passkey \"" + name + "\" registered for " + owner + " from " +
         String(caller.peer.ToString()) + ".");

      return BuildResponse_(201, PasskeyJson(passkey));
   }

   HttpResponse
   RestApiServer::HandlePasskeyChallenge_(const AnsiString &request, const IPAddress &peer_address)
   {
      if (!PasskeyStore::Enabled())
         return BuildResponse_(403, "{\"error\":\"passkeys are switched off on this server\"}");

      const AnsiString relyingParty = PasskeyStore::RelyingParty(GetHeader_(request, "host"));
      if (relyingParty.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"this server cannot work out a relying party id for the host this page was served on; set passkey_relying_party\"}");

      const AnsiString missingOrigins = PasskeyStore::MissingOriginsProblem();
      if (!missingOrigins.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(missingOrigins) + "\"}");

      AnsiString nonce, challenge;
      if (!PasskeyStore::OfferSignIn(nonce, challenge))
         return BuildResponse_(503, "{\"error\":\"no challenge could be made; try again\"}");

      // Path=/api/v1 so the sign-in carries it and nothing else does;
      // SameSite=Strict because the page that asked is this server's own;
      // Secure whenever the browser came over HTTPS (BrowserCameOverTls_).
      AnsiString cookie;
      cookie.Format("Set-Cookie: %hs=%hs; Path=/api/v1; HttpOnly; SameSite=Strict; Max-Age=%d%hs\r\n",
         PasskeyCookieName, nonce.c_str(), PasskeyStore::ChallengeSeconds, BrowserCameOverTls_(request, peer_address) ? "; Secure" : "");

      AnsiString body;
      body.Format("{\"challenge\":\"%hs\",\"rp_id\":\"%hs\",\"timeout\":%d}",
         challenge.c_str(), JsonEscape_(relyingParty).c_str(), ChallengeMilliseconds);

      return BuildResponse_(200, body, cookie);
   }

   // ------------------------------------------------------------------------
   // The sign-in itself, called from Authenticate_ for a POST /api/v1/session
   // whose body carries a passkey instead of a password. Everything it decides
   // it decides from the request: there is no session yet, and the cookie it
   // reads is the one the challenge was handed out with.
   // ------------------------------------------------------------------------
   bool
   RestApiServer::AuthenticatePasskey_(const AnsiString &request, const IPAddress &peer_address, Caller &caller)
   {
      if (!PasskeyStore::Enabled())
         return false;

      const AnsiString requestBody = GetRequestBody_(request);
      JsonValue body;
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
         return false;

      const JsonValue *assertion = body.Get("passkey");
      const JsonValue *response = assertion == nullptr ? nullptr : assertion->Get("response");
      if (response == nullptr)
         return false;

      std::string clientDataJson, authenticatorData, signature;
      if (!Binary(response, "clientDataJSON", clientDataJson) ||
          !Binary(response, "authenticatorData", authenticatorData) ||
          !Binary(response, "signature", signature))
         return false;

      // The challenge this browser was given, sealed in its cookie; spent below,
      // once an assertion over it has verified.
      AnsiString nonce;
      std::vector<AnsiString> cookies = StringParser::SplitString(GetHeader_(request, "cookie"), ";");
      for (AnsiString part : cookies)
      {
         part.Trim();
         if (part.StartsWith(AnsiString(PasskeyCookieName) + "="))
            nonce = part.Mid((int) strlen(PasskeyCookieName) + 1);
      }

      AnsiString challenge;
      if (!PasskeyStore::TakeSignIn(nonce, challenge))
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " carried no challenge this server had issued to it.");
         return false;
      }

      AnsiString problem;
      WebAuthn::ClientData clientData;
      if (!WebAuthn::ReadClientData(clientDataJson, clientData, problem) ||
          clientData.type != "webauthn.get" ||
          AnsiString(clientData.challenge.c_str()) != challenge)
         return false;

      // The origin, held to this server's own (PasskeyStore::OriginAllowed).
      // Compared with the Host header only while the relying party id follows
      // that header, where a page at another host cannot get an assertion the
      // rp id hash below would take; once PasskeyRelyingParty names a parent
      // domain, every sibling host under it can ask the browser for one, and
      // the Host header is the attacker's to write - so the origin must then be
      // one PasskeyAllowedOrigins names. And not a frame of another site's page.
      AnsiString wantedHost = GetHeader_(request, "host");
      wantedHost.MakeLower();
      AnsiString originProblem;
      if (clientData.cross_origin ||
          !PasskeyStore::OriginAllowed(AnsiString(clientData.origin.c_str()), HostOfOrigin(clientData.origin), wantedHost, originProblem))
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " was refused: " + String(clientData.cross_origin ? AnsiString("it was collected in a frame of another site's page") : originProblem) + ".");
         return false;
      }

      const AnsiString relyingParty = PasskeyStore::RelyingParty(wantedHost);
      if (relyingParty.IsEmpty())
         return false;

      const std::string credentialId = assertion->GetString("id");
      PasskeyStore::Passkey passkey;
      if (credentialId.empty() || !PasskeyStore::FindByCredential(AnsiString(credentialId.c_str()), passkey))
         return false;

      WebAuthn::AuthenticatorData authData;
      if (!WebAuthn::ReadAuthenticatorData(authenticatorData, authData, problem) ||
          authData.rp_id_hash != WebAuthn::Sha256(Bytes(relyingParty)))
         return false;

      // User present AND user verified, both, exactly as the registration
      // demanded both. The one-time code is not asked for on top of a passkey,
      // and the whole of the reason is that the authenticator verified the
      // person - a PIN, a fingerprint, a face - before it signed. A credential
      // enrolled as two factors that could later assert with a touch alone
      // would be one factor at the moment it matters: a stolen security key,
      // driven by a client that asks for no verification, signing its way into
      // a mailbox whose password sign-in would have demanded a code. The
      // browser is asked for userVerification "required" on both pages, and
      // this is the same demand made where it cannot be talked out of.
      if ((authData.flags & WebAuthn::FlagUserPresent) == 0 ||
          (authData.flags & WebAuthn::FlagUserVerified) == 0)
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " was made by an authenticator that did not verify the person; it is refused, because a passkey stands in for the one-time code only when it does.");
         return false;
      }

      std::string cose;
      WebAuthn::PublicKey key;
      if (!WebAuthn::Base64UrlDecode(Bytes(passkey.public_key), cose) ||
          !WebAuthn::ReadPublicKey(cose, key, problem) ||
          !WebAuthn::VerifySignature(key, authenticatorData, clientDataJson, signature))
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " did not verify against the stored key.");
         RegisterAuthenticationFailure_(peer_address);
         return false;
      }

      // The clone check. An authenticator that keeps a counter moves it on with
      // every assertion, so a counter that has not moved is either a copy of
      // the credential or a replay - and one that reports 0 always keeps no
      // counter at all, which is allowed and is not evidence of anything.
      if (!(authData.sign_count == 0 && passkey.sign_count == 0) && (__int64) authData.sign_count <= passkey.sign_count)
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " reported a signature counter that had not moved on; it is refused as a possible clone of the credential.");
         RegisterAuthenticationFailure_(peer_address);
         return false;
      }

      // The assertion verified: its challenge is spent, once. Only a verified
      // assertion spends one, so a stranger cannot fill the spent set; and a
      // second assertion over the same challenge - a replay, or the same one
      // posted twice at once - is refused here.
      if (!PasskeyStore::SpendSignIn(challenge))
      {
         LOG_APPLICATION("REST API: a passkey sign-in from " + String(peer_address.ToString()) +
            " answered a challenge that had been answered already.");
         return false;
      }

      // A discoverable credential names its owner. When it does, it has to be
      // the owner the credential id already named.
      const std::string handle = response->GetString("userHandle");
      if (!handle.empty())
      {
         __int64 handleAccount = 0;
         bool handleAdministrator = false;
         if (!PasskeyStore::OwnerOfHandle(handle, handleAccount, handleAdministrator) ||
             handleAdministrator != passkey.administrator || handleAccount != passkey.account_id)
            return false;
      }

      if (passkey.administrator)
      {
         caller.result = AuthenticatedAsAdministrator;
         caller.read_only = false;
         caller.identity = "administrator";
         caller.grant = RoleManager::ServerAdministrator();
         PasskeyStore::RecordUse(passkey.id, (__int64) authData.sign_count);
         LOG_APPLICATION("REST API: the administrator signed in with a passkey from " + String(peer_address.ToString()) + ".");
         return true;
      }

      std::shared_ptr<Account> loaded = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(loaded, passkey.account_id))
         return false;

      // The hold, the country policy and the last-logon stamp, which is where
      // every logon that proves identity without a password meets them.
      std::shared_ptr<const Account> account = loaded;
      if (!AccountMayStartSession_(account, peer_address))
         return false;

      caller.result = AuthenticatedAsAccount;
      caller.read_only = false;
      caller.identity = AnsiString("account:") + AnsiString(account->GetAddress());
      caller.account = account;
      caller.via_passkey = true;
      // What the device list shows beside "passkey": the name its owner gave
      // this authenticator, which is the question that list exists to answer -
      // which of my credentials is this client using.
      caller.credential_name = passkey.name;
      ApplyAccountGrant_(caller);

      PasskeyStore::RecordUse(passkey.id, (__int64) authData.sign_count);
      LOG_APPLICATION("REST API: " + account->GetAddress() + " signed in with a passkey from " + String(peer_address.ToString()) + ".");
      return true;
   }
}
