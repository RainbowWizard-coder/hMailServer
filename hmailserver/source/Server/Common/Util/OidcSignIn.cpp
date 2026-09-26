// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "OidcSignIn.h"
#include "HttpsClient.h"
#include "JsonDocument.h"
#include "JwksKeySet.h"
#include "OAuth2TokenValidator.h"
#include "ReaderOAuth.h"
#include "Unicode.h"
#include "WebAuthn.h"
#include "Crypt.h"
#include "../Application/Configuration.h"
#include "../Application/IniFileSettings.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include <ctime>
#include <map>
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
      const time_t PendingSeconds = 10 * 60;
      const size_t MostPending = 200;
      const int DiscoverySeconds = 3600;
      const int JwksCacheSeconds = 3600;
      const int ClockSkewSeconds = 60;

      struct Discovery
      {
         AnsiString issuer;
         AnsiString authorize;
         AnsiString token;
         String jwks;
         time_t fetched = 0;
      };

      struct Pending
      {
         bool deck = false;
         AnsiString verifier;
         AnsiString cookie;
         AnsiString nonce;
         time_t created = 0;
         // Which one this was, counted under the lock: created is accurate to
         // the second and a flood makes hundreds inside one, so this is what
         // "the oldest" means when a full table has to make room.
         unsigned __int64 order = 0;
      };

      struct Ticket
      {
         OidcSignIn::Result result;
         AnsiString binding;   // the hmsso cookie the browser was given with it
         time_t created = 0;
         unsigned __int64 order = 0;
      };

      boost::mutex stateLock;
      Discovery discovery;
      std::map<AnsiString, Pending> pending;
      std::map<AnsiString, Ticket> tickets;
      unsigned __int64 nextOrder = 1;

      // The sign-in's own signing-key cache, apart from the one the token
      // validator uses (JwksKeySet::Instance(), OAuth2JwksUrl). One JwksKeySet
      // holds one URL, and a GetKey for another drops its keys and its failure
      // hold: shared, a server using both would fetch again on every switch
      // between an id_token and a bearer token - synchronously, under the
      // set's lock, which every token check waits on - and, with the identity
      // provider down, would try it again on each switch rather than holding
      // off for FailureHoldSeconds.
      JwksKeySet &SigningKeys()
      {
         // Never destroyed, as the Singleton one is not: the keys it holds are
         // OpenSSL's, and no exit-time destructor frees them after OpenSSL has gone.
         static JwksKeySet *keys = new JwksKeySet();
         return *keys;
      }

      AnsiString Utf8(const String &text)
      {
         AnsiString bytes;
         Unicode::WideToMultiByte(text, bytes);
         return bytes;
      }

      String FromUtf8(const std::string &bytes)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(bytes.c_str()), text);
         return text;
      }

      AnsiString Setting(const TCHAR *name)
      {
         AnsiString value = Utf8(Configuration::Instance()->GetSignInString(name));
         value.Trim();
         return value;
      }

      AnsiString Trimmed(AnsiString text)
      {
         text.Trim();
         while (text.EndsWith("/"))
            text = text.Mid(0, text.GetLength() - 1);
         return text;
      }

      bool RandomText(AnsiString &text)
      {
         unsigned char bytes[32];
         if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return false;
         text = WebAuthn::Base64Url(bytes, sizeof(bytes));
         return true;
      }

      bool SameText(const AnsiString &a, const AnsiString &b)
      {
         return a.GetLength() == b.GetLength() && a.GetLength() > 0 &&
                CRYPTO_memcmp(a.c_str(), b.c_str(), (size_t) a.GetLength()) == 0;
      }

      void PruneLocked(time_t now)
      {
         for (auto it = pending.begin(); it != pending.end();)
         {
            if (now - it->second.created > PendingSeconds)
               it = pending.erase(it);
            else
               ++it;
         }

         for (auto it = tickets.begin(); it != tickets.end();)
         {
            if (now - it->second.created > OidcSignIn::TicketSeconds)
               it = tickets.erase(it);
            else
               ++it;
         }
      }

      // A full table drops its oldest entry and takes the new one. /portal/oidc/start
      // carries no credential - the person has not signed in anywhere yet - so a
      // table that refused when it filled would let a stranger switch single
      // sign-on off for everybody with two hundred requests, and the sentence
      // the sign-in page would then show blames the issuer. Evicting costs the
      // flooder their own unfinished sign-ins instead.
      template <typename Table>
      void MakeRoomLocked(Table &table)
      {
         while (table.size() >= MostPending)
         {
            auto oldest = table.begin();
            for (auto it = table.begin(); it != table.end(); ++it)
            {
               if (it->second.order < oldest->second.order)
                  oldest = it;
            }
            table.erase(oldest);
         }
      }

      // The issuer's own description of itself. Cached for an hour: it names
      // three endpoints that change about as often as the issuer's certificate,
      // and fetching it on every sign-in would be a request per person per day
      // against somebody else's server.
      bool Discover(Discovery &answer, AnsiString &problem)
      {
         const AnsiString issuer = Trimmed(Setting(PROPERTY_OIDC_ISSUER));
         if (issuer.IsEmpty())
         {
            problem = "no identity provider is configured on this server";
            return false;
         }

         {
            boost::lock_guard<boost::mutex> guard(stateLock);
            if (discovery.issuer == issuer && time(nullptr) - discovery.fetched < DiscoverySeconds &&
                !discovery.authorize.IsEmpty())
            {
               answer = discovery;
               return true;
            }
         }

         HttpsClient::Response response;
         String error;
         std::vector<AnsiString> headers;
         headers.push_back("Accept: application/json");

         if (!HttpsClient::Request("GET", issuer + "/.well-known/openid-configuration", headers, "", "",
                                   response, error, 20, 256 * 1024))
         {
            problem = "the identity provider could not be reached: " + AnsiString(error);
            return false;
         }

         JsonValue document;
         std::string parseError;
         std::string body(response.body.c_str(), (size_t) response.body.GetLength());
         if (response.status_code != 200 || !JsonValue::Parse(body, document, parseError) || !document.IsObject())
         {
            problem.Format("the identity provider's discovery document could not be read (%d)", response.status_code);
            return false;
         }

         Discovery fresh;
         fresh.issuer = AnsiString(document.GetString("issuer").c_str());
         fresh.authorize = AnsiString(document.GetString("authorization_endpoint").c_str());
         fresh.token = AnsiString(document.GetString("token_endpoint").c_str());
         fresh.jwks = FromUtf8(document.GetString("jwks_uri"));
         fresh.fetched = time(nullptr);

         if (fresh.issuer.IsEmpty() || fresh.authorize.IsEmpty() || fresh.token.IsEmpty() || fresh.jwks.IsEmpty())
         {
            problem = "the identity provider's discovery document names no issuer, authorize endpoint, token endpoint or key set";
            return false;
         }

         // The document's own issuer is what the id_token has to match, and it
         // has to be the issuer this server was pointed at: a document served
         // at one issuer that declares another is either a misconfiguration or
         // somebody else's directory.
         if (Trimmed(fresh.issuer) != issuer)
         {
            problem = "the identity provider's discovery document declares a different issuer";
            return false;
         }

         {
            boost::lock_guard<boost::mutex> guard(stateLock);
            discovery = fresh;
         }

         answer = fresh;
         return true;
      }

      bool VerifyWithKey(const AnsiString &signingInput, const AnsiString &signature, EVP_PKEY *key)
      {
         if (key == nullptr)
            return false;

         bool verified = false;
         EVP_MD_CTX *ctx = EVP_MD_CTX_new();
         if (ctx != nullptr)
         {
            if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                EVP_DigestVerifyUpdate(ctx, signingInput.c_str(), signingInput.GetLength()) == 1)
            {
               verified = EVP_DigestVerifyFinal(ctx, (const unsigned char *) signature.c_str(),
                                                signature.GetLength()) == 1;
            }
            EVP_MD_CTX_free(ctx);
         }

         return verified;
      }

      // The id_token, checked rather than read. See OidcSignIn.h.
      bool VerifyIdToken(const std::string &idToken, const Discovery &provider, const AnsiString &clientId,
                         const AnsiString &nonce, JsonValue &claims, AnsiString &problem)
      {
         const size_t first = idToken.find('.');
         const size_t second = first == std::string::npos ? std::string::npos : idToken.find('.', first + 1);
         if (second == std::string::npos || idToken.find('.', second + 1) != std::string::npos)
         {
            problem = "the identity provider's id_token is not a JWT";
            return false;
         }

         AnsiString headerBytes, payloadBytes, signature;
         if (!OAuth2TokenValidator::Base64UrlDecode(AnsiString(idToken.substr(0, first).c_str()), headerBytes) ||
             !OAuth2TokenValidator::Base64UrlDecode(AnsiString(idToken.substr(first + 1, second - first - 1).c_str()), payloadBytes) ||
             !OAuth2TokenValidator::Base64UrlDecode(AnsiString(idToken.substr(second + 1).c_str()), signature))
         {
            problem = "the id_token is not base64url";
            return false;
         }

         JsonValue header;
         std::string parseError;
         std::string headerText(headerBytes.c_str(), (size_t) headerBytes.GetLength());
         std::string payloadText(payloadBytes.c_str(), (size_t) payloadBytes.GetLength());
         if (!JsonValue::Parse(headerText, header, parseError) || !header.IsObject() ||
             !JsonValue::Parse(payloadText, claims, parseError) || !claims.IsObject())
         {
            problem = "the id_token's header or payload is not JSON";
            return false;
         }

         const AnsiString algorithm = AnsiString(header.GetString("alg").c_str());
         const AnsiString kid = AnsiString(header.GetString("kid").c_str());

         // RS256 and ES256 only. "none" would make the token anybody's to
         // write, and HS256 would make it anybody's who holds the client
         // secret - including, at a provider that reuses it, another tenant.
         const int keyType = algorithm == "RS256" ? EVP_PKEY_RSA : algorithm == "ES256" ? EVP_PKEY_EC : 0;
         if (keyType == 0)
         {
            problem = "the id_token is signed with an algorithm this server does not accept (" + algorithm.Mid(0, 16) + ")";
            return false;
         }

         EVP_PKEY *key = nullptr;
         AnsiString keyError;
         if (!SigningKeys().GetKey(provider.jwks, JwksCacheSeconds, kid, keyType, &key, keyError) || key == nullptr)
         {
            problem = "the identity provider's signing key could not be read: " + keyError;
            return false;
         }

         const AnsiString signingInput = AnsiString(idToken.substr(0, second).c_str());

         // JWS carries an ECDSA signature as the raw R||S pair and OpenSSL
         // verifies X9.62 DER; the transcode is the validator's, tested there.
         AnsiString der;
         const bool verified = keyType == EVP_PKEY_EC
            ? (OAuth2TokenValidator::RawEcdsaSignatureToDer_(signature, der) && VerifyWithKey(signingInput, der, key))
            : VerifyWithKey(signingInput, signature, key);
         EVP_PKEY_free(key);

         if (!verified)
         {
            problem = "the id_token's signature did not verify against the identity provider's key";
            return false;
         }

         if (AnsiString(claims.GetString("iss").c_str()) != provider.issuer)
         {
            problem = "the id_token was issued by somebody else";
            return false;
         }

         // aud is a string or an array of them; this server's client id has to
         // be in it either way.
         bool forUs = AnsiString(claims.GetString("aud").c_str()) == clientId;
         if (!forUs)
         {
            const JsonValue *audience = claims.Get("aud");
            for (size_t i = 0; audience != nullptr && audience->IsArray() && i < audience->Size(); i++)
            {
               const JsonValue *one = audience->At(i);
               if (one != nullptr && one->IsString() && AnsiString(one->AsString().c_str()) == clientId)
                  forUs = true;
            }
         }
         if (!forUs)
         {
            problem = "the id_token was not issued for this server";
            return false;
         }

         const __int64 now = (__int64) time(nullptr);
         const __int64 expires = claims.GetInt64("exp");
         if (expires <= 0 || expires + ClockSkewSeconds < now)
         {
            problem = "the id_token has expired";
            return false;
         }

         const __int64 notBefore = claims.GetInt64("nbf");
         const __int64 issuedAt = claims.GetInt64("iat");
         if ((notBefore > 0 && notBefore - ClockSkewSeconds > now) || (issuedAt > 0 && issuedAt - ClockSkewSeconds > now))
         {
            problem = "the id_token is not valid yet";
            return false;
         }

         if (!SameText(AnsiString(claims.GetString("nonce").c_str()), nonce))
         {
            problem = "the id_token answers a different sign-in";
            return false;
         }

         return true;
      }

      // The addresses the list names - OidcAdministrators, or SAML's own -
      // compared case-insensitively.
      bool IsAdministratorAddress(const String &address, const String &property)
      {
         const String list = Configuration::Instance()->GetSignInString(property);
         if (list.IsEmpty())
            return false;

         std::vector<String> parts = StringParser::SplitString(list, _T(","));
         for (String part : parts)
         {
            part.Trim();
            if (!part.IsEmpty() && part.CompareNoCase(address) == 0)
               return true;
         }
         return false;
      }

      bool OidcAutoProvision()
      {
         return Configuration::Instance()->GetOidcAutoProvision();
      }
   }

   bool
   OidcSignIn::Configured()
   {
      return !Setting(PROPERTY_OIDC_ISSUER).IsEmpty() &&
             !Setting(PROPERTY_OIDC_CLIENT_ID).IsEmpty() &&
             !RedirectUri().IsEmpty();
   }

   AnsiString
   OidcSignIn::Label()
   {
      AnsiString issuer = Trimmed(Setting(PROPERTY_OIDC_ISSUER));
      if (issuer.IsEmpty())
         return "";

      const int scheme = issuer.Find("://");
      if (scheme >= 0)
         issuer = issuer.Mid(scheme + 3);

      const int slash = issuer.Find("/");
      if (slash >= 0)
         issuer = issuer.Mid(0, slash);

      return issuer;
   }

   AnsiString
   OidcSignIn::RedirectUri()
   {
      AnsiString base = Trimmed(Setting(PROPERTY_OIDC_REDIRECT_BASE_URL));
      if (base.IsEmpty())
         base = Trimmed(Utf8(Configuration::Instance()->GetImportOAuthRedirectBaseUrl()));
      if (base.IsEmpty())
         return "";
      return base + "/portal/oidc/callback";
   }

   bool
   OidcSignIn::Begin(bool deck, AnsiString &url, AnsiString &cookie, AnsiString &problem)
   {
      if (!Configured())
      {
         problem = "signing in with an identity provider is not set up on this server";
         return false;
      }

      Discovery provider;
      if (!Discover(provider, problem))
         return false;

      Pending entry;
      AnsiString state;
      if (!RandomText(state) || !RandomText(entry.verifier) || !RandomText(entry.cookie) || !RandomText(entry.nonce))
      {
         problem = "the random generator failed";
         return false;
      }

      entry.deck = deck;
      entry.created = time(nullptr);

      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         entry.order = nextOrder++;
         PruneLocked(entry.created);
         MakeRoomLocked(pending);
         pending[state] = entry;
      }

      AnsiString scopes = Setting(PROPERTY_OIDC_SCOPES);
      if (scopes.IsEmpty())
         scopes = "openid email";

      url = provider.authorize + (provider.authorize.Find("?") >= 0 ? "&" : "?") +
            "response_type=code" +
            "&client_id=" + HttpsClient::FormEncode(Setting(PROPERTY_OIDC_CLIENT_ID)) +
            "&redirect_uri=" + HttpsClient::FormEncode(RedirectUri()) +
            "&scope=" + HttpsClient::FormEncode(scopes) +
            "&state=" + state +
            "&nonce=" + entry.nonce +
            "&code_challenge=" + ReaderOAuth::Challenge(entry.verifier) +
            "&code_challenge_method=S256";
      cookie = entry.cookie;
      return true;
   }

   bool
   OidcSignIn::Complete(const AnsiString &state, const AnsiString &cookie, const AnsiString &code,
                        Result &result, AnsiString &problem)
   {
      Pending entry;
      {
         boost::lock_guard<boost::mutex> guard(stateLock);
         auto it = pending.find(state);
         if (it == pending.end())
         {
            problem = "this sign-in was not started here, or it was finished already";
            return false;
         }
         entry = it->second;
         pending.erase(it);
      }

      result = Result();
      result.deck = entry.deck;

      if (time(nullptr) - entry.created > PendingSeconds)
      {
         problem = "the sign-in took longer than ten minutes; start it again";
         return false;
      }
      if (!SameText(entry.cookie, cookie))
      {
         problem = "this sign-in was started in another browser; start it again from the sign-in page";
         return false;
      }
      if (code.IsEmpty())
      {
         problem = "the identity provider sent no code back";
         return false;
      }

      Discovery provider;
      if (!Discover(provider, problem))
         return false;

      const AnsiString clientId = Setting(PROPERTY_OIDC_CLIENT_ID);
      AnsiString form = "grant_type=authorization_code&code=" + HttpsClient::FormEncode(code) +
                        "&redirect_uri=" + HttpsClient::FormEncode(RedirectUri()) +
                        "&client_id=" + HttpsClient::FormEncode(clientId) +
                        "&code_verifier=" + HttpsClient::FormEncode(entry.verifier);

      const AnsiString secret = Setting(PROPERTY_OIDC_CLIENT_SECRET);
      if (!secret.IsEmpty())
         form += "&client_secret=" + HttpsClient::FormEncode(secret);

      HttpsClient::Response response;
      String error;
      std::vector<AnsiString> headers;
      headers.push_back("Accept: application/json");

      if (!HttpsClient::Request("POST", provider.token, headers, "application/x-www-form-urlencoded", form,
                                response, error, 20, 256 * 1024))
      {
         problem = "the identity provider could not be reached: " + AnsiString(error);
         return false;
      }

      JsonValue answer;
      std::string parseError;
      std::string body(response.body.c_str(), (size_t) response.body.GetLength());
      if (!JsonValue::Parse(body, answer, parseError) || !answer.IsObject())
      {
         problem.Format("the identity provider answered %d, and not with JSON", response.status_code);
         return false;
      }

      if (response.status_code != 200)
      {
         std::string reason = answer.GetString("error_description", answer.GetString("error", "no reason given"));
         problem = AnsiString(("the identity provider refused: " + reason.substr(0, 240)).c_str());
         return false;
      }

      const std::string idToken = answer.GetString("id_token");
      if (idToken.empty())
      {
         problem = "the identity provider answered no id_token, so nothing here says who signed in";
         return false;
      }

      JsonValue claims;
      if (!VerifyIdToken(idToken, provider, clientId, entry.nonce, claims, problem))
         return false;

      // An email claim the issuer has not verified is a string somebody typed
      // at the provider. Treating it as an identity is how one person signs in
      // as another, so it is refused - and said so plainly, since an issuer
      // that does not verify addresses cannot be used for this at all.
      // Some issuers write the claim as the string "true" rather than the JSON
      // boolean; both are the issuer saying it verified the address, and
      // anything else - including its absence - is not.
      const bool verifiedAddress = claims.GetBool("email_verified", false) ||
                                   claims.GetString("email_verified") == "true";
      if (!verifiedAddress)
      {
         problem = "the identity provider did not say the address is verified, so it cannot name a mailbox here";
         return false;
      }

      String address = FromUtf8(claims.GetString("email"));
      address.Trim();
      if (address.IsEmpty() || address.Find(_T("@")) < 0 || address.GetLength() > 255)
      {
         problem = "the identity provider did not say which address signed in";
         return false;
      }

      Mapping mapping;
      mapping.administrators = PROPERTY_OIDC_ADMINISTRATORS;
      mapping.auto_provision = &OidcAutoProvision;
      mapping.auto_provision_name = PROPERTY_OIDC_AUTO_PROVISION;
      mapping.component = _T("OidcSignIn");

      return MapAddress(entry.deck, address, mapping, result, problem);
   }

   bool
   OidcSignIn::MapAddress(bool deck, const String &address, const Mapping &mapping, Result &result, AnsiString &problem)
   {
      result.address = address;

      // The Control Deck: the administrator, and only for an address the list
      // names. An issuer that says somebody is an administrator is not obeyed.
      if (deck)
      {
         if (!IsAdministratorAddress(address, mapping.administrators))
         {
            problem = "that address does not open the Control Deck on this server";
            return false;
         }
         result.administrator = true;
         return true;
      }

      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (PersistentAccount::ReadObject(account, address) && account->GetID() > 0)
      {
         if (!account->GetActive())
         {
            problem = "that mailbox is not active on this server";
            return false;
         }

         // Its domain as well, as a password sign-in asks it: a domain an
         // administrator has switched off opens to nobody, however they
         // proved who they are.
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());
         if (!domain || !domain->GetIsActive())
         {
            problem = "that mailbox's domain is not active on this server";
            return false;
         }

         result.account_id = account->GetID();
         return true;
      }

      if (mapping.auto_provision == nullptr || !mapping.auto_provision())
      {
         problem = "that address has no mailbox on this server";
         return false;
      }

      const String domainName = StringParser::ExtractDomain(address);
      Domains domains;
      domains.Refresh();
      std::shared_ptr<Domain> domain = domains.GetItemByName(domainName);
      if (!domain || !domain->GetIsActive())
      {
         problem = "that address is at a domain this server does not have";
         return false;
      }

      // A password of thirty-two random bytes that nobody is ever told: the
      // account exists so the person can sign in at the issuer, and until an
      // administrator sets a password it can be signed in to no other way.
      // No password policy runs, because nobody chose this.
      unsigned char secretBytes[32];
      if (RAND_bytes(secretBytes, sizeof(secretBytes)) != 1)
      {
         problem = "the random generator failed, so no mailbox was made";
         return false;
      }

      const String unusable = String(WebAuthn::Base64Url(secretBytes, sizeof(secretBytes)));
      const int hashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();

      std::shared_ptr<Account> made = std::shared_ptr<Account>(new Account());
      made->SetDomainID(domain->GetID());
      made->SetAddress(address);
      made->SetActive(true);
      made->SetPassword(Crypt::Instance()->EnCrypt(unusable, (Crypt::EncryptionType) hashAlgorithm));
      made->SetPasswordEncryption(hashAlgorithm);

      String saveError;
      if (!PersistentAccount::SaveObject(made, saveError, true, PersistenceModeNormal) || made->GetID() == 0)
      {
         problem = saveError.IsEmpty() ? AnsiString("the mailbox could not be made; see the error log") : Utf8(saveError);
         return false;
      }

      LOG_APPLICATION(mapping.component + _T(": made a mailbox for ") + address + _T(", which the identity provider verified and ") +
                      mapping.auto_provision_name + _T(" allows."));

      result.account_id = made->GetID();
      return true;
   }

   bool
   OidcSignIn::MintTicket(const Result &result, AnsiString &ticket, AnsiString &binding)
   {
      if (!RandomText(ticket) || !RandomText(binding))
         return false;

      Ticket entry;
      entry.result = result;
      entry.binding = binding;
      entry.created = time(nullptr);

      boost::lock_guard<boost::mutex> guard(stateLock);
      entry.order = nextOrder++;
      PruneLocked(entry.created);
      MakeRoomLocked(tickets);
      tickets[ticket] = entry;
      return true;
   }

   bool
   OidcSignIn::RedeemTicket(const AnsiString &ticket, const AnsiString &binding, Result &result)
   {
      if (ticket.IsEmpty())
         return false;

      boost::lock_guard<boost::mutex> guard(stateLock);
      const time_t now = time(nullptr);
      PruneLocked(now);

      auto it = tickets.find(ticket);
      if (it == tickets.end())
         return false;

      // The browser the ticket was handed to, and no other. A ticket is a
      // string, and a string can be carried: somebody who completes a sign-in
      // of their own and sends the link it ended at - /portal#sso=<ticket> -
      // to another person would otherwise have that person's browser, through
      // this server's own page, redeem it and be signed into the sender's
      // mailbox (login forgery). The cookie was set on the same answer that
      // carried the ticket, so only the browser that finished the sign-in has
      // it. A mismatch leaves the ticket where it is: whoever presented it
      // without its cookie holds nothing that spending it would take away.
      if (!SameText(it->second.binding, binding))
         return false;

      result = it->second.result;
      tickets.erase(it);
      return true;
   }

   // Path=/api/v1/session, so that the one request that redeems a ticket
   // carries it and nothing else does; SameSite=Strict, because that request
   // is made by this server's own page.
   AnsiString
   OidcSignIn::TicketCookieHeader(const AnsiString &binding, bool secure)
   {
      AnsiString header;
      header.Format("Set-Cookie: %hs=%hs; Path=/api/v1/session; HttpOnly; SameSite=Strict; Max-Age=%d%hs\r\n",
         TicketCookieName, binding.c_str(), TicketSeconds, secure ? "; Secure" : "");
      return header;
   }

   AnsiString
   OidcSignIn::ClearTicketCookieHeader(bool secure)
   {
      AnsiString header;
      header.Format("Set-Cookie: %hs=; Path=/api/v1/session; HttpOnly; SameSite=Strict; Max-Age=0%hs\r\n",
         TicketCookieName, secure ? "; Secure" : "");
      return header;
   }

   AnsiString
   OidcSignIn::TicketBindingFrom(const AnsiString &cookieHeader)
   {
      const AnsiString prefix = AnsiString(TicketCookieName) + "=";
      AnsiString value;
      std::vector<AnsiString> cookies = StringParser::SplitString(cookieHeader, ";");
      for (AnsiString part : cookies)
      {
         part.Trim();
         if (part.StartsWith(prefix))
            value = part.Mid(prefix.GetLength());
      }
      return value;
   }

   void
   OidcSignIn::Clear()
   {
      boost::lock_guard<boost::mutex> guard(stateLock);
      discovery = Discovery();
      pending.clear();
      tickets.clear();
      SigningKeys().Clear();
   }
}
