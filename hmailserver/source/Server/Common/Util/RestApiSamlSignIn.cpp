// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// SAML sign-in over the listener (SamlSignIn.h), beside OIDC's
// (RestApiOidcSignIn.cpp):
//
//    GET  /portal/saml/start[?deck=1]   send the browser to the identity provider,
//                                       with the hmsaml cookie
//    POST /portal/saml/acs              the Response it sends the browser back with
//    GET  /portal/saml/finish           where the ACS sends the browser on, and
//                                       where the cookie is asked for
//    GET  /saml/metadata                this service provider, for the identity
//                                       provider to import
//
// None of them carries a credential and none of them mints a session. The ACS
// verifies the Response and keeps what it verified under a one-use token; the
// finish, reached by the same browser with the cookie the start gave it, mints
// OIDC's ticket - thirty-two random bytes, good once and for two minutes - and
// sends the browser to the page with it in the URL fragment, where the page's
// existing single sign-on code hands it to POST /api/v1/session exactly as it
// does after OIDC. One ticket, one redeemer (AuthenticateOidcTicket_), one place
// a browser session is made.
//
// Why the finish is a step of its own: the identity provider's POST to the ACS
// is cross-site, so no SameSite=Lax cookie rides it, and a Response is a thing
// its owner can carry to another person's browser. The GET the ACS answers
// with (303, since the browser came with a POST) is a navigation to this
// server, and the Lax cookie does ride that - so it is there, and not at the
// ACS, that the browser proves it is the one that started the sign-in.
#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "HttpsClient.h"
#include "OidcSignIn.h"
#include "SamlSignIn.h"
#include "../Application/Logger.h"

#include <cstring>
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
      // The browser's half of a SAML sign-in, set by the start and asked for by
      // the finish. SameSite=Lax: it must ride the top-level GET the ACS sends
      // the browser on to, and it cannot ride the identity provider's POST to
      // the ACS whatever it says, which is why the ACS does not ask for it.
      const char *SamlCookieName = "hmsaml";

      AnsiString CookieValue(const AnsiString &cookieHeader)
      {
         AnsiString value;
         std::vector<AnsiString> cookies = StringParser::SplitString(cookieHeader, ";");
         for (AnsiString part : cookies)
         {
            part.Trim();
            if (part.StartsWith(AnsiString(SamlCookieName) + "="))
               value = part.Mid((int) strlen(SamlCookieName) + 1);
         }
         return value;
      }
   }

   AnsiString
   RestApiServer::OpenApiSamlPaths_()
   {
      static const char *paths =
         ",\"/portal/saml/start\":{\"get\":{\"summary\":\"Start a sign-in at the organisation's SAML identity provider\",\"description\":\"No credential. Redirects to saml_idp_sso_url with an AuthnRequest in the SAMLRequest parameter (raw DEFLATE, base64, the HTTP-Redirect binding; not signed) and a RelayState, and sets the hmsaml cookie (HttpOnly, SameSite=Lax, Path=/portal/saml, ten minutes; Secure over TLS or behind a loopback proxy that says X-Forwarded-Proto: https) the finish asks for. The request's ID is kept for ten minutes, one use, beside the RelayState and the cookie; the Response must answer it. ?deck=1 starts one for the Control Deck, which lands as the administrator when the address is in saml_administrators; without it the sign-in is the webmail's. Redirects back to the sign-in page with #sso_error=<sentence> when SAML sign-in is not set up.\",\"responses\":{\"302\":{\"description\":\"To the identity provider, or back to the page with an error\"}}}},"
         "\"/portal/saml/acs\":{\"post\":{\"summary\":\"Where the SAML identity provider has the browser post its Response\",\"description\":\"No credential. A form body of SAMLResponse (base64 of the Response) and RelayState, the HTTP-POST binding, at most 256 KB (413 otherwise). The Response is verified against saml_idp_certificate: an enveloped XML signature, exclusive canonicalisation, SHA-256, rsa-sha256 or ecdsa-sha256, the Reference naming the signed element's own ID, the element used re-read from the signed bytes; one Assertion, signed itself unless saml_require_signed_assertion is off; Destination and Recipient this ACS, InResponseTo a request issued here and unused with its RelayState, Issuer the identity provider, the audience this service provider, NotBefore and NotOnOrAfter with a minute of skew, the assertion not seen before. An encrypted assertion, an IdP-initiated Response, a DTD, an entity, an XSLT or XPath transform, SHA-1 and a second assertion are refused. On success the browser is sent on to /portal/saml/finish with a one-use token; otherwise back to the page with #sso_error=<sentence>.\",\"responses\":{\"303\":{\"description\":\"On to the finish, or back to the page with an error\"},\"413\":{\"description\":\"The body is larger than 256 KB\"}}}},"
         "\"/portal/saml/finish\":{\"get\":{\"summary\":\"Where the ACS sends the browser on to finish a SAML sign-in\",\"description\":\"No credential. ?token= names a sign-in the ACS verified in the last two minutes, and the browser must carry the hmsaml cookie the start gave for that request - so a Response carried to another browser ends here. The token is spent either way. The address - an emailAddress NameID, else the attribute saml_email_attribute names - is then mapped as OIDC maps one, and the browser is sent to the page with a one-use ticket in the URL fragment for POST /api/v1/session; otherwise with #sso_error=<sentence>, to the page the sign-in was started for.\",\"responses\":{\"303\":{\"description\":\"Back to the page, with a ticket or an error\"}}}},"
         "\"/saml/metadata\":{\"get\":{\"summary\":\"This server's SAML service provider metadata\",\"description\":\"No credential. An EntityDescriptor with one SPSSODescriptor: this service provider's entity ID (saml_sp_entity_id, else the base URL and /saml) and its ACS, <base URL>/portal/saml/acs, on the HTTP-POST binding; AuthnRequestsSigned false, since requests are not signed, and so no KeyDescriptor. For the identity provider to import. 404 while no base URL is set (oidc_redirect_base_url, or import_oauth_redirect_base_url).\",\"responses\":{\"200\":{\"description\":\"The metadata, application/samlmetadata+xml\"},\"404\":{\"description\":\"No base URL is set\"}}}}";
      return AnsiString(paths);
   }

   bool
   RestApiServer::BrowserCameOverTls_(const AnsiString &request, const IPAddress &peer_address) const
   {
      if (use_tls_)
         return true;

      // Start refuses a plain-HTTP listener anywhere but the loopback address,
      // so the only proxy that can stand in front of one is on this machine -
      // and it is the one that may say the browser came over HTTPS.
      const AnsiString peer = peer_address.ToString();
      if (peer != "::1" && !peer.StartsWith("127."))
         return false;

      AnsiString protocol = GetHeader_(request, "x-forwarded-proto");
      const int comma = protocol.Find(",");
      if (comma >= 0)
         protocol = protocol.Mid(0, comma);
      protocol.Trim();
      protocol.MakeLower();
      return protocol == "https";
   }

   HttpResponse
   RestApiServer::HandleSamlStart_(const AnsiString &request, const AnsiString &query, const IPAddress &peer_address)
   {
      const bool deck = QueryParameter_(query, "deck") == "1";
      const AnsiString page = deck ? AnsiString("/") : AnsiString("/portal");

      AnsiString url, cookie, problem;
      if (!SamlSignIn::Begin(deck, url, cookie, problem))
      {
         return BuildResponse_(302, "{}", "Location: " + page + "#sso_error=" + HttpsClient::FormEncode(problem) +
            "\r\nReferrer-Policy: no-referrer\r\n");
      }

      AnsiString headers;
      headers.Format("Set-Cookie: %hs=%hs; Path=/portal/saml; HttpOnly; SameSite=Lax; Max-Age=%d%hs\r\n"
                     "Referrer-Policy: no-referrer\r\n",
         SamlCookieName, cookie.c_str(), (int) SamlSignIn::PendingSeconds,
         BrowserCameOverTls_(request, peer_address) ? "; Secure" : "");

      return BuildResponse_(302, "{}", "Location: " + url + "\r\n" + headers);
   }

   HttpResponse
   RestApiServer::HandleSamlAcs_(const AnsiString &request)
   {
      // The HTTP-POST binding: a form, the two fields percent-encoded as a form
      // encodes them.
      const AnsiString body = GetRequestBody_(request);
      const AnsiString samlResponse = QueryParameter_(body, "SAMLResponse");
      const AnsiString relayState = QueryParameter_(body, "RelayState");

      OidcSignIn::Result result;
      AnsiString finishToken, problem;
      if (!SamlSignIn::Complete(samlResponse, relayState, result, finishToken, problem))
      {
         // Which page is decided by this server's record of the sign-in, never
         // by the Response (SamlSignIn::Complete).
         LOG_APPLICATION("RestApi: a SAML sign-in did not complete: " + String(problem) + ".");
         return BuildResponse_(303, "{}", "Location: " + (result.deck ? AnsiString("/") : AnsiString("/portal")) +
            "#sso_error=" + HttpsClient::FormEncode(problem) + "\r\nReferrer-Policy: no-referrer\r\n");
      }

      // On to the finish, on this server: a navigation, which the hmsaml
      // cookie rides.
      return BuildResponse_(303, "{}", "Location: /portal/saml/finish?token=" + finishToken +
         "\r\nReferrer-Policy: no-referrer\r\n");
   }

   HttpResponse
   RestApiServer::HandleSamlFinish_(const AnsiString &request, const AnsiString &query, const IPAddress &peer_address)
   {
      // The cookie has done its work whatever happens next.
      AnsiString headers;
      headers.Format("Set-Cookie: %hs=; Path=/portal/saml; HttpOnly; SameSite=Lax; Max-Age=0%hs\r\n"
                     "Referrer-Policy: no-referrer\r\n",
         SamlCookieName, BrowserCameOverTls_(request, peer_address) ? "; Secure" : "");

      auto back = [&headers](const OidcSignIn::Result &result, const AnsiString &fragment)
      {
         return BuildResponse_(303, "{}", "Location: " + (result.deck ? AnsiString("/") : AnsiString("/portal")) + "#" +
            fragment + "\r\n" + headers);
      };

      OidcSignIn::Result result;
      AnsiString problem;
      if (!SamlSignIn::Finish(QueryParameter_(query, "token"), CookieValue(GetHeader_(request, "cookie")), result, problem))
      {
         LOG_APPLICATION("RestApi: a SAML sign-in did not finish: " + String(problem) + ".");
         return back(result, "sso_error=" + HttpsClient::FormEncode(problem));
      }

      AnsiString ticket;
      if (!OidcSignIn::MintTicket(result, ticket))
         return back(result, "sso_error=" + HttpsClient::FormEncode(AnsiString("the sign-in could not be finished here; try again")));

      LOG_APPLICATION("RestApi: " + result.address + " was verified by the SAML identity provider and may now start a " +
         String(result.deck ? _T("Control Deck") : _T("webmail")) + " session.");

      return back(result, "sso=" + ticket);
   }

   HttpResponse
   RestApiServer::HandleSamlMetadata_()
   {
      const AnsiString metadata = SamlSignIn::Metadata();
      if (metadata.IsEmpty())
      {
         return BuildResponse_(404, "{\"error\":\"No base URL is set (oidc_redirect_base_url, or import_oauth_redirect_base_url), "
                                    "so this server has no address for an identity provider to send anybody back to.\"}");
      }

      HttpResponse response = BuildResponse_(200, metadata);
      response.content_type = "application/samlmetadata+xml";
      return response;
   }
}
