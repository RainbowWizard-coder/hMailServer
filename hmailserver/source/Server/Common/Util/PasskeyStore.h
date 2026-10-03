// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The passkeys this server holds, and the challenges a registration or a
// sign-in is answered against.
//
// A passkey is a WebAuthn credential: the private key never leaves the person's
// authenticator (a phone, a laptop's secure enclave, a security key), and what
// this server keeps is the public half, the credential id that names it, and a
// signature counter. That is one row of hm_passkeys (schema 6077), whose
// account id is the mailbox it belongs to - or NULL, which is the
// administrator's own, since the administrator is a password in
// hMailServer.ini and not an account.
//
// WHAT IS KEPT AND WHY
//
//   pkcredential   the credential id, base64url, unique across the server - it
//                  is what an assertion names itself by, and two accounts
//                  cannot share one.
//   pkpublickey    the COSE key exactly as the authenticator wrote it, base64url.
//   pkalgorithm    -7 (ES256) or -257 (RS256). Nothing else is registered,
//                  because nothing else can be verified here.
//   pksigncount    the counter the authenticator last reported. An assertion
//                  whose counter has not moved past it, when the authenticator
//                  keeps one at all, is the signature of a CLONED credential
//                  and is refused (WebAuthn level 3, 6.1.3 step 18).
//   pkname         what the person called the device.
//   pktransports   what the browser said about how it is reached (usb, nfc,
//                  ble, internal, hybrid), shown in the list and used as a hint.
//
// THE CHALLENGES are in memory and nowhere else: a registration's is held
// against the session that asked for it, and a sign-in's against a nonce the
// browser carries in a short-lived cookie, because a sign-in has no session
// yet. Both last five minutes and both are spent on first use, so a captured
// assertion cannot be replayed. A server restart forgets them, which costs a
// person one press of the button.
//
// The store is not in the backup. A passkey is bound to the relying party id -
// the host the page is served on - and to an authenticator that will sign for
// that id and no other, so an archive restored onto a different host carries
// credentials that could never be used there; and the administrator's rows are
// not an account's to carry. AccountStores' elsewhere_ says so.
#pragma once

#include <string>
#include <vector>

namespace HM
{
   class PasskeyStore
   {
   public:

      struct Passkey
      {
         __int64 id = 0;
         __int64 account_id = 0;      // 0 with administrator = the administrator's own
         bool administrator = false;
         AnsiString credential;       // base64url
         AnsiString public_key;       // base64url of the COSE key
         int algorithm = 0;
         __int64 sign_count = 0;
         String name;
         String transports;
         AnsiString aaguid;           // base64url, 16 octets
         __int64 created = 0;
         __int64 last_used = 0;
      };

      // Whether passkeys are offered at all (PasskeysEnabled).
      static bool Enabled();

      // The relying party id a page served on this host binds a passkey to: the
      // PasskeyRelyingParty setting when one is set, and otherwise the Host
      // header with its port taken off. Empty for a host this server cannot
      // make sense of, which refuses the registration rather than binding the
      // credential to something the browser will not accept.
      static AnsiString RelyingParty(const AnsiString &host);

      // Whether PasskeyRelyingParty is set - a relying party id that does not
      // follow the page's host.
      static bool RelyingPartyConfigured();

      // The origins a passkey may be registered and used at: the
      // PasskeyAllowedOrigins setting, each written scheme://host[:port],
      // lower-cased, without a trailing slash. Empty when it is not set.
      //
      // Why it exists. The origin in clientDataJSON is what the browser says
      // the page that asked for the credential was, and it is the one thing an
      // attacker cannot write; the Host header is. With the relying party id
      // following the Host header, an origin that matches the Host header also
      // fixes the relying party the credential is bound to, and a page at
      // another host can get nothing a passkey of this server's would sign. With
      // PasskeyRelyingParty set to a parent domain (example.com), a page at ANY
      // host under it may ask the browser for a passkey of example.com - so the
      // origin has to be one this server names as its own, never one the
      // request itself names. OriginAllowed is that check, for both the
      // registration and the sign-in.
      static std::vector<AnsiString> AllowedOrigins();

      // Whether a passkey collected at origin (clientDataJSON's, exactly as the
      // browser wrote it) may be taken by a request whose Host header is host.
      // When AllowedOrigins is set, or PasskeyRelyingParty is, the origin must
      // be in the list - and with a relying party and no list, nothing is
      // taken; otherwise it must be the page's own host. problem says which
      // for the log.
      static bool OriginAllowed(const AnsiString &origin, const AnsiString &originHost, const AnsiString &host,
                                AnsiString &problem);

      // The sentence the challenge routes answer while PasskeyRelyingParty is
      // set and PasskeyAllowedOrigins is not; empty otherwise.
      static AnsiString MissingOriginsProblem();

      // The opaque handle the credential carries for its owner, and the way
      // back from one. "admin" for the administrator, "a<account id>" for an
      // account - stable, so a person who re-registers the same authenticator
      // replaces the credential rather than gaining a second one, and holding
      // nothing the account's own address does not already say.
      static AnsiString UserHandle(__int64 accountId, bool administrator);
      static bool OwnerOfHandle(const std::string &handle, __int64 &accountId, bool &administrator);

      static constexpr int MostPerOwner = 10;
      static constexpr int ChallengeSeconds = 300;

      // The rows one owner has, newest first.
      static bool List(__int64 accountId, bool administrator, std::vector<Passkey> &passkeys);

      // The row a credential id names, whoever owns it.
      static bool FindByCredential(const AnsiString &credential, Passkey &passkey);

      // Adds one. problem carries a sentence for the person when it fails; the
      // unique index on the credential id is what stops the same credential
      // being registered twice, and that case is reported as its own sentence.
      static bool Add(Passkey &passkey, String &problem);

      // Removes one of this owner's. False when it is not theirs, or gone.
      static bool Remove(__int64 accountId, bool administrator, __int64 id);

      // After a sign-in: the counter the authenticator reported, and now.
      static void RecordUse(__int64 id, __int64 signCount);

      // The registration challenge, held against the session that asked, and
      // spent by the registration that follows.
      // holder is the credential asking (RestApiServer's caller identity): whom
      // the challenge counts against when the table makes room, fifty a holder
      // (SignInBudget.h).
      static bool OfferRegistration(const std::string &holder, const AnsiString &sessionKey, AnsiString &challenge);
      static bool TakeRegistration(const AnsiString &sessionKey, AnsiString &challenge);

      // The sign-in challenge. There is no session yet, so the browser carries
      // it in a cookie: nonce is the challenge and when it ends, sealed
      // (SignInState.h), and nothing is held here - no number of challenges
      // asked for, from wherever, can push anybody's out. TakeSignIn answers
      // the challenge a nonce holds while it is in time and not spent;
      // SpendSignIn spends it, once, when an assertion over it has verified -
      // true the first time, false after.
      static bool OfferSignIn(AnsiString &nonce, AnsiString &challenge);
      static bool TakeSignIn(const AnsiString &nonce, AnsiString &challenge);
      static bool SpendSignIn(const AnsiString &challenge);

      // Forgets every challenge. For Reinitialize and the tests.
      static void Clear();
   };

   // The challenges' own behaviour, which has nothing to do with the database: a
   // sign-in challenge is answered by its own sealed nonce and spent once, and
   // any number asked for leaves a person's answerable; a flooded registration
   // table evicts rather than refuses - the flooding credential's own
   // challenges, not anybody else's (SignInBudget.h). The flood is the property
   // a stranger would otherwise be able to turn into a switch, so it is proved
   // on every debug start.
   class PasskeyStoreTester
   {
   public:
      void Test();
   };
}
