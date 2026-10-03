// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The pending half of the sign-ins that begin with no credential at all - an
// OpenID Connect start (OidcSignIn), a SAML start (SamlSignIn) and a passkey
// sign-in's challenge (PasskeyStore) - carried by the browser, sealed, instead
// of held in a table here.
//
// WHY. Each start is answered to anybody, so whatever a start leaves on the
// server is something a stranger can make it hold. Until 6.3.6 each kind sat in
// a table that dropped its oldest entry when full, so a client asking for
// starts a few times a second pushed every other person's sign-in out before
// their browser came back; 6.3.6 first held each source to its own share
// (SignInBudget.h), which a pool of addresses as large as the table still beat
// - a few thousand /24s, which rotating proxy services sell. Now a start leaves
// nothing behind: what the callback needs - the PKCE verifier, the nonce, which
// page the sign-in is for, the SAML request's ID, when it ends - travels in the
// hmoidc, hmsaml or hmpasskey cookie, sealed with AES-256-GCM under a key of
// this process's (Seal), and comes back with the browser. There is no table a
// stranger can fill, so there is nothing for anybody's start to push out.
//
// WHAT IS STILL KEPT HERE: what makes each sign-in good once. A state that
// finished is spent (Spend) - recorded until it would have expired, in a set
// that only a finished sign-in adds to: the identity provider's verified answer,
// or a passkey's verified signature. A stranger cannot add to it without
// signing in. Bounded all the same (MostSpent a kind), dropping the soonest to
// expire past that.
//
// THE KEY is 32 random bytes made the first time it is needed and again at every
// reinitialise and stop (Clear), held in memory and written nowhere - the
// lifetime the tables had, which a sign-in in flight across a restart never
// survived either. A key kept beyond it would let a state sealed before a
// reinitialise, whose spent record went with it, be used a second time. Two
// nodes behind one name each have their own, as each had its own table.
//
// The SAML request travels through the identity provider, which echoes its ID
// in the Response, and its RelayState beside it; the hmsaml cookie does not
// ride the provider's cross-site POST to the ACS. So the request's ID carries
// its own Tag - what it is for, when it ends - and the ACS checks the tag
// without anything held here.
#pragma once

#include <ctime>
#include <string>

namespace HM
{
   class SignInState
   {
   public:
      // The kinds, each its own associated data and spent set.
      static constexpr const char *Oidc = "oidc";
      static constexpr const char *SamlCookie = "saml-cookie";
      static constexpr const char *SamlRequest = "saml-request";
      static constexpr const char *Passkey = "passkey";

      // How many spent states a kind holds at most.
      static const size_t MostSpent = 16384;

      // plain sealed for kind, as cookie-safe text: base64url of the nonce, the
      // cipher text and the tag. Empty when sealing failed.
      static AnsiString Seal(const char *kind, const std::string &plain);

      // What Seal sealed for kind under this process's key; false for anything
      // else - another kind, a changed character, a state sealed before a Clear,
      // text that is not one, text longer than any state.
      static bool Open(const char *kind, const AnsiString &sealed, std::string &plain);

      // A tag of data for kind: 16 bytes of HMAC-based PRF output (HKDF-SHA256,
      // RFC 5869) under this process's key, base64url. Compared in constant
      // time by Verify.
      static AnsiString Tag(const char *kind, const std::string &data);
      static bool Verify(const char *kind, const std::string &data, const AnsiString &tag);

      // Records id of kind as spent until expires (seconds since the epoch).
      // True the first time; false when it was spent already.
      static bool Spend(const char *kind, const std::string &id, time_t expires);
      static bool IsSpent(const char *kind, const std::string &id);

      // A new key, and nothing spent: every state sealed before is refused.
      static void Clear();
   };

   // Seal and Open round-tripped, and their refusals: another kind, a changed
   // character, a state from before a Clear; Tag and Verify; Spend once. No
   // database, configuration, file or network. ClassTester runs it on Windows,
   // sign-in-budget-tests on Linux.
   class SignInStateTester
   {
   public:
      void Test();
   };
}
