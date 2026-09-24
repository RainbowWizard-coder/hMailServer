// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Importing from the old provider with a sign-in: the reader's side of a move
// from Gmail or Outlook.com, which no longer take a password from another mail
// server (Outlook.com not at all, Gmail only an app password). The reader
// presses a button in the webmail, signs in at the provider, and comes back to
// an external account that collects with the grant it gave - over IMAP, with
// XOAUTH2, bringing the folders over when they ask for that.
//
// The flow is the OAuth 2.0 authorization code grant with PKCE (RFC 7636, S256):
//
//   Begin      a sign-in is started for one account of this server. A state
//              (the key of what is pending), a verifier and a nonce are drawn
//              from the random generator; the page is sent to the provider's
//              authorize endpoint with the state and the verifier's challenge,
//              and the browser is given the nonce as a cookie.
//   Complete   the provider sends the browser back to /portal/oauth/callback
//              with the code and the state. The state must name a sign-in
//              started in the last ten minutes, and the browser must carry the
//              nonce that sign-in was given - so a link someone else sends
//              cannot attach their mailbox to this reader's account, which is
//              the attack a callback without it is open to. The pending entry
//              is removed whatever happens next: a state works once. The code
//              is exchanged at the token endpoint with the verifier, and the
//              address comes from the id_token that answer carries - read, not
//              verified, because it came straight from the provider over TLS
//              in answer to this server's own request (OpenID Connect Core
//              3.1.3.7 allows exactly that).
//   AccessToken the fetcher's: an access token for a fetch account that holds
//              a provider and a refresh token, kept until four fifths of its
//              life has passed, refreshed with the refresh token otherwise. A
//              provider that rotates the refresh token (Microsoft does) has the
//              new one written back to the fetch account at once.
//
// The providers are registered by the administrator, not by this project: a
// client id and secret per provider, and the base URL the provider sends the
// reader back to (the listener's public address). A provider without them is
// not offered. The endpoints are settings too, with the public ones as their
// defaults - a national cloud has its own, and the regression tests point them
// at a local server.
#pragma once

namespace HM
{
   class FetchAccount;

   class ReaderOAuth
   {
   public:
      struct Provider
      {
         const char *word;               // google, microsoft
         const char *label;              // Gmail, Outlook.com
         const TCHAR *client_id_property;
         const TCHAR *client_secret_property;
         const TCHAR *authorize_url_property;
         const TCHAR *token_url_property;
         const char *scope;
         const char *extra_authorize_parameters;
         bool secret_required;
         const char *imap_host;
         int imap_port;
      };

      // The provider a word names, or null.
      static const Provider *Find(const AnsiString &word);

      // Whether the administrator has registered this server with the provider:
      // a client id, a secret where the provider requires one, both endpoints,
      // and the base URL the reader is sent back to.
      static bool Configured(const Provider &provider);

      // <base URL>/portal/oauth/callback, or empty when no base URL is set.
      static AnsiString RedirectUri();

      // Starts a sign-in for an account. url is where the page sends the
      // browser; nonce is what the browser keeps in a cookie until it comes
      // back. mirror and daysToKeep are the reader's choices, kept with the
      // sign-in for the fetch account made at the end of it.
      static bool Begin(__int64 accountId, const Provider &provider, bool mirror, int daysToKeep,
                        AnsiString &url, AnsiString &nonce, AnsiString &problem);

      // A migration job's sign-in (Application/MigrationManager.h): the same
      // flow, begun from a per-job link the owner of a mailbox being migrated
      // opens, for a mailbox that is not an account of this server. scope is
      // what the job needs consent for - mail, contacts and calendars at once
      // (MigrationSources' table) - and redeemScope, when not empty, is the one
      // resource the code is redeemed for, which Microsoft requires of a
      // consent that spans two. linkDigest is the digest of the link the flow
      // was begun from, carried to the end so that a flow begun from a link
      // since replaced is refused. Complete answers the job and the digest in
      // the grant.
      static bool BeginForMigration(__int64 jobId, const AnsiString &linkDigest, const Provider &provider, const AnsiString &scope,
                                    const AnsiString &redeemScope, AnsiString &url, AnsiString &nonce, AnsiString &problem);

      // A bearer token for one scope from a refresh token that is not a fetch
      // account's - a migration job's, which asks for the mail, the contacts and
      // the calendars in turn. grantedScope is what the provider says the token
      // carries (empty when it did not say), so a phase whose scope was not
      // granted is skipped with that reason; rotatedRefreshToken is a
      // replacement the provider issued, for the caller to keep. Not cached: a
      // job asks once per phase. transient is set when the provider could not
      // answer now - no answer at all, a 429, a 5xx, or temporarily_unavailable
      // or server_error - which a later attempt may not meet, as against a
      // grant it refuses, which only its owner signing in again mends.
      static bool RefreshAccessToken(const Provider &provider, const AnsiString &refreshToken, const AnsiString &scope,
                                     String &token, AnsiString &grantedScope, AnsiString &rotatedRefreshToken,
                                     bool &transient, String &problem);

      struct Grant
      {
         Grant() : account_id(0), migration_job_id(0), mirror(false), days_to_keep(-1) {}

         __int64 account_id;
         __int64 migration_job_id;   // a migration job's sign-in, and then account_id is 0
         AnsiString migration_link;  // the digest of the link that job's sign-in was begun from
         AnsiString provider;
         String address;
         AnsiString refresh_token;
         bool mirror;
         int days_to_keep;
      };

      // Completes a sign-in: the state and the code the provider sent back,
      // the nonce the browser carried. False with problem - one sentence the
      // page can show - when anything does not match or the provider refuses.
      static bool Complete(const AnsiString &state, const AnsiString &nonce, const AnsiString &code,
                           Grant &grant, AnsiString &problem);

      // A bearer token for a fetch account signed in with a provider.
      static bool AccessToken(std::shared_ptr<FetchAccount> fetchAccount, String &token, String &problem);

      // Forgets a fetch account's cached token - the server refused it.
      static void Forget(__int64 fetchAccountId);

      // base64url without padding, and its reverse. Public for ReaderOAuthTester.
      static AnsiString Base64Url(const unsigned char *data, size_t length);
      static bool Base64UrlDecode(const std::string &text, std::string &bytes);

      // The S256 challenge of a verifier: base64url(SHA-256(verifier)).
      static AnsiString Challenge(const AnsiString &verifier);

      // The address an id_token names: email, else preferred_username when it
      // is an address. Empty when there is none or the token is not a JWT.
      static String AddressOfIdToken(const std::string &idToken);
   };

   class ReaderOAuthTester
   {
   public:
      void Test();
   };
}
