// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Single sign-on over the listener (OidcSignIn.h):
//
//    GET /portal/oidc/start[?deck=1]   send the browser to the issuer
//    GET /portal/oidc/callback         where the issuer sends it back
//    GET /api/v1/signin-methods        what the two sign-in pages may offer
//
// None of the three carries a credential, and none of them mints a session.
// The callback verifies the id_token and then mints a TICKET - thirty-two
// random bytes, good once and for two minutes - and sends the browser to the
// page with it in the URL fragment, which a browser sends to no server and no
// access log holds. The page hands the ticket to POST /api/v1/session, which
// is where every browser session of this server is made: the same table, the
// same policy, the same device record, whether the person proved themselves
// with a password, a passkey or their organisation's identity provider.
//
// That is the reason for the extra step. A callback that set the session
// cookie itself would be a second place a session is minted, with its own copy
// of the idle ceiling, the one-per-device rule and the bind-to-address rule -
// and the copy would be the one that drifted.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "HttpsClient.h"
#include "OidcSignIn.h"
#include "PasskeyStore.h"
#include "SamlSignIn.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"

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
      // The browser's half of the state. SameSite=Lax, not Strict: the browser
      // comes back from the issuer's site, which is a top-level navigation from
      // another origin - Lax lets the cookie ride and Strict does not.
      const char *OidcCookieName = "hmoidc";
   }

   AnsiString
   RestApiServer::OpenApiOidcPaths_()
   {
      static const char *paths =
         ",\"/portal/oidc/start\":{\"get\":{\"summary\":\"Start a sign-in at the organisation's identity provider\",\"description\":\"No credential. Redirects to the issuer's authorization endpoint with the authorization-code flow and PKCE (S256), and sets the hmoidc cookie (HttpOnly, SameSite=Lax, ten minutes) the callback needs. ?deck=1 starts one for the Control Deck, which lands as the administrator when the address is in OidcAdministrators; without it the sign-in is the webmail's. Redirects back to the sign-in page with ?sso_error=<sentence> when single sign-on is not set up.\",\"responses\":{\"302\":{\"description\":\"To the issuer, or back to the page with an error\"}}}},"
         "\"/portal/oidc/callback\":{\"get\":{\"summary\":\"Where the identity provider sends the browser back\",\"description\":\"No credential. The state must name a sign-in started here in the last ten minutes and the browser must carry the cookie it was given. The code is exchanged at the token endpoint, the id_token is verified against the issuer's JWK Set (signature, iss, aud, exp, nonce) and its verified email is mapped to a local account - or to the administrator, for a Deck sign-in whose address is in OidcAdministrators. On success the browser is sent to the page with a one-use ticket in the URL fragment, which the page exchanges at POST /api/v1/session; otherwise back with ?sso_error=<sentence>.\",\"responses\":{\"302\":{\"description\":\"Back to the page, with a ticket or an error\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleSignInMethods_()
   {
      // SAML's flag and label beside OIDC's (SamlSignIn.h): the two are
      // separate buttons, since an installation may front both.
      AnsiString body;
      body.Format("{\"passkeys\":%hs,\"oidc\":%hs,\"oidc_label\":\"%hs\",\"saml\":%hs,\"saml_label\":\"%hs\"}",
         PasskeyStore::Enabled() ? "true" : "false",
         OidcSignIn::Configured() ? "true" : "false",
         JsonEscape_(OidcSignIn::Label()).c_str(),
         SamlSignIn::Configured() ? "true" : "false",
         JsonEscape_(SamlSignIn::Label()).c_str());
      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleOidcStart_(const AnsiString &query)
   {
      const bool deck = QueryParameter_(query, "deck") == "1";
      const AnsiString page = deck ? AnsiString("/") : AnsiString("/portal");

      AnsiString url, cookie, problem;
      if (!OidcSignIn::Begin(deck, url, cookie, problem))
      {
         return BuildResponse_(302, "{}", "Location: " + page + "#sso_error=" +
            HttpsClient::FormEncode(problem) + "\r\nCache-Control: no-store\r\n");
      }

      AnsiString headers;
      headers.Format("Set-Cookie: %hs=%hs; Path=/portal/oidc; HttpOnly; SameSite=Lax; Max-Age=600%hs\r\n"
                     "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n",
         OidcCookieName, cookie.c_str(), use_tls_ ? "; Secure" : "");

      return BuildResponse_(302, "{}", "Location: " + url + "\r\n" + headers);
   }

   HttpResponse
   RestApiServer::HandleOidcCallback_(const AnsiString &request, const AnsiString &query)
   {
      AnsiString headers;
      headers.Format("Set-Cookie: %hs=; Path=/portal/oidc; HttpOnly; SameSite=Lax; Max-Age=0%hs\r\n"
                     "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n",
         OidcCookieName, use_tls_ ? "; Secure" : "");

      // Which page the browser goes back to is decided by the sign-in that was
      // started, never by the query string: a link somebody sends must not be
      // able to point a webmail sign-in at the Control Deck.
      auto back = [&headers](const AnsiString &page, const AnsiString &fragment)
      {
         return BuildResponse_(302, "{}", "Location: " + page + "#" + fragment + "\r\n" + headers);
      };

      AnsiString cookie;
      std::vector<AnsiString> cookies = StringParser::SplitString(GetHeader_(request, "cookie"), ";");
      for (AnsiString part : cookies)
      {
         part.Trim();
         if (part.StartsWith(AnsiString(OidcCookieName) + "="))
            cookie = part.Mid((int) strlen(OidcCookieName) + 1);
      }

      const AnsiString state = QueryParameter_(query, "state");
      const AnsiString code = QueryParameter_(query, "code");
      const AnsiString issuerError = QueryParameter_(query, "error");

      OidcSignIn::Result result;
      AnsiString problem;
      if (!OidcSignIn::Complete(state, cookie, issuerError.IsEmpty() ? code : AnsiString(), result, problem))
      {
         if (!issuerError.IsEmpty())
            problem = "the identity provider did not complete the sign-in (" + issuerError.Mid(0, 60) + ")";

         // The page the sign-in was for is not known when the state was not
         // found, which is exactly when there is nothing to know: the webmail's
         // own sign-in page is the safe place to say so.
         const AnsiString page = result.deck ? AnsiString("/") : AnsiString("/portal");
         LOG_APPLICATION("RestApi: a single sign-on did not complete: " + String(problem) + ".");
         return back(page, "sso_error=" + HttpsClient::FormEncode(problem));
      }

      AnsiString ticket;
      if (!OidcSignIn::MintTicket(result, ticket))
      {
         return back(result.deck ? AnsiString("/") : AnsiString("/portal"),
                     "sso_error=" + HttpsClient::FormEncode(AnsiString("the sign-in could not be finished here; try again")));
      }

      LOG_APPLICATION("RestApi: " + result.address + " was verified by the identity provider and may now start a " +
         String(result.deck ? _T("Control Deck") : _T("webmail")) + " session.");

      return back(result.deck ? AnsiString("/") : AnsiString("/portal"), "sso=" + ticket);
   }

   // The ticket redeemed, from Authenticate_, for a POST /api/v1/session whose
   // body carries one instead of a password.
   bool
   RestApiServer::AuthenticateOidcTicket_(const AnsiString &request, const IPAddress &peer_address, Caller &caller)
   {
      const AnsiString ticket = JsonUtf8Value_(GetRequestBody_(request), "sso_ticket");
      if (ticket.IsEmpty())
         return false;

      OidcSignIn::Result result;
      if (!OidcSignIn::RedeemTicket(ticket, result))
      {
         LOG_APPLICATION("REST API: a single sign-on ticket from " + String(peer_address.ToString()) +
            " was not one this server had issued, or it had been spent.");
         return false;
      }

      if (result.administrator)
      {
         caller.result = AuthenticatedAsAdministrator;
         caller.read_only = false;
         caller.identity = "administrator";
         caller.grant = RoleManager::ServerAdministrator();
         LOG_APPLICATION("REST API: " + result.address + " signed in as the administrator through the identity provider from " +
            String(peer_address.ToString()) + ".");
         return true;
      }

      std::shared_ptr<Account> loaded = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(loaded, result.account_id))
         return false;

      // The same two gates a passkey sign-in meets, for the same reason: the
      // issuer says who this is, and this server says whether that name may
      // sign in at all. An account the compromised-account detection has
      // locked, or one signing in from a country its domain refuses, is
      // refused here whatever the issuer thinks of it - and the last-logon
      // stamp moves on, which it would not if the only way somebody ever
      // signed in was this one.
      std::shared_ptr<const Account> account = loaded;
      if (!AccountMayStartSession_(account, peer_address))
         return false;

      caller.result = AuthenticatedAsAccount;
      caller.read_only = false;
      caller.identity = AnsiString("account:") + AnsiString(account->GetAddress());
      caller.account = account;
      caller.via_single_sign_on = true;
      ApplyAccountGrant_(caller);

      LOG_APPLICATION("REST API: " + account->GetAddress() + " signed in through the identity provider from " +
         String(peer_address.ToString()) + ".");
      return true;
   }
}
