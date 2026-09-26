// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Single sign-on: the organisation's identity provider as the front door to the
// webmail and the Control Deck.
//
// It is the flow ReaderOAuth already runs against Gmail and Outlook.com - the
// OAuth 2.0 authorization code grant with PKCE (RFC 7636, S256) - pointed at
// the issuer an administrator names, with one difference that is the whole
// point: the id_token is VERIFIED here rather than read. ReaderOAuth may read
// it unverified because it came straight from the provider over TLS in answer
// to this server's own request (OpenID Connect Core 3.1.3.7 says so), and the
// worst a wrong address there does is attach the wrong mailbox to the reader's
// own import. Here the id_token decides who somebody is, so it is checked:
//
//   signature   against the issuer's published JWK Set (JwksKeySet), by the kid
//               the token names. RS256 and ES256; "none" and HS256 are refused,
//               since a token this server accepted on a symmetric algorithm
//               would be one anybody holding the client secret could write.
//   iss         exactly the issuer the discovery document declares.
//   aud         contains this server's client id.
//   exp         in the future, and iat/nbf not in the future (with a minute of
//               skew, which is what every implementation allows).
//   nonce       the one this server put in the authorize request, held against
//               the state, so a token minted for another sign-in is refused.
//
// Then the token's email is mapped to a local account, and only when the token
// says that address is verified - an unverified email claim is a string the
// person typed at the provider, and treating it as an identity is how one
// account signs in as another.
//
// WHAT IT DOES NOT DO
//
//   * No account is created for an address this server does not have unless
//     OidcAutoProvision is on, and then only at a domain this server already
//     has. A directory of ten thousand people is not ten thousand mailboxes.
//   * The access token and the refresh token are thrown away. This is a
//     sign-in, not a grant to act at the provider on the person's behalf, and
//     keeping a refresh token this server never uses would be a credential
//     kept for nothing.
//   * No groups, no roles, no claims mapping. What a person may do here is
//     decided by this server's own roles, against the account the address
//     mapped to. An issuer that says somebody is an administrator is not
//     obeyed: the addresses that open the Control Deck are the list in
//     OidcAdministrators, which an administrator of THIS server wrote.
//   * There is no back-channel or front-channel logout. Signing out of the
//     webmail ends the session here; the session at the issuer is the issuer's.
//   * One issuer. An installation that fronts two directories needs two, and
//     that is not built.
#pragma once

#include <string>

namespace HM
{
   class OidcSignIn
   {
   public:

      // Whether an administrator has named an issuer and a client id, and a
      // base URL for the issuer to send the browser back to.
      static bool Configured();

      // What the sign-in pages show on the button: the issuer's host, which is
      // the only name this server is sure of. Empty when it is not configured.
      static AnsiString Label();

      // <base URL>/portal/oidc/callback. Empty when no base URL is set.
      static AnsiString RedirectUri();

      // Starts a sign-in. deck is true for the Control Deck and false for the
      // webmail: the two come back to the same callback and are told apart by
      // what the state remembers, so one link cannot be used to reach the other
      // page. url is where the browser is sent; cookie is what it must carry
      // back.
      static bool Begin(bool deck, AnsiString &url, AnsiString &cookie, AnsiString &problem);

      struct Result
      {
         bool deck = false;
         bool administrator = false;   // the address is in OidcAdministrators
         __int64 account_id = 0;       // 0 for the administrator
         String address;
      };

      // Finishes one: the state and the code the issuer sent back, and the
      // cookie the browser carried. On success the address has been verified
      // and mapped; problem is one sentence for the page otherwise.
      static bool Complete(const AnsiString &state, const AnsiString &cookie, const AnsiString &code,
                           Result &result, AnsiString &problem);

      // Whose the settings are that decide what a verified address may open:
      // OIDC's, or SAML's (SamlSignIn.h), which keeps its own list and its
      // own switch and otherwise the same rules.
      struct Mapping
      {
         // The hm_settings property listing the addresses that open the
         // Control Deck, comma separated.
         String administrators;

         // Whether an unknown address at a domain this server has gets a
         // mailbox: read only when one is about to be made, as OIDC always
         // read it.
         bool (*auto_provision)();

         // That switch's name and the protocol's, for the log line.
         String auto_provision_name;
         String component;
      };

      // Who a verified address is here, the rules both protocols share: for
      // the Control Deck, the administrator when the list names the address
      // and a refusal otherwise; for the webmail, the active mailbox at that
      // address, or - with the switch on and at a domain this server has - a
      // mailbox made for it with a password nobody is told. result.deck is the
      // caller's; address, administrator and account_id are set here.
      static bool MapAddress(bool deck, const String &address, const Mapping &mapping, Result &result, AnsiString &problem);

      // A verified sign-in, held for two minutes and spent once, so that the
      // session itself is still minted by POST /api/v1/session and by nothing
      // else. The ticket travels in the URL fragment, which a browser does not
      // send to any server and no access log holds. A SAML sign-in ends in the
      // same ticket, redeemed by the same route: one ticket, one redeemer.
      //
      // And redeemed only by the browser it was handed to. binding is a second
      // random value, set as the TicketCookieName cookie on the same redirect
      // that carries the ticket and held with it here; RedeemTicket refuses a
      // ticket presented without it. Without that, a ticket is a bearer string
      // that its owner can send to somebody else as a link, and this server's
      // own page would redeem it in that person's browser - signing them into
      // the sender's mailbox.
      static bool MintTicket(const Result &result, AnsiString &ticket, AnsiString &binding);
      static bool RedeemTicket(const AnsiString &ticket, const AnsiString &binding, Result &result);

      static const int TicketSeconds = 120;

      // HttpOnly, SameSite=Strict, Path=/api/v1/session, TicketSeconds long.
      static constexpr const char *TicketCookieName = "hmsso";

      // The Set-Cookie line that hands binding to the browser, and the one
      // that takes it away again once the ticket is spent. Secure when the
      // browser came over TLS.
      static AnsiString TicketCookieHeader(const AnsiString &binding, bool secure);
      static AnsiString ClearTicketCookieHeader(bool secure);

      // The binding a request's Cookie header carries; empty when none.
      static AnsiString TicketBindingFrom(const AnsiString &cookieHeader);

      // Forgets the discovery document, the pending sign-ins and the tickets.
      // For Reinitialize and the tests.
      static void Clear();
   };
}
