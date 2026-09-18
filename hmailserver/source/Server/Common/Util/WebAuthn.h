// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The reader for what a WebAuthn authenticator sends: the client data, the
// authenticator data, the CBOR attestation object, the COSE public key inside
// it, and the signature verification an assertion is judged by. No database, no
// settings, no HTTP - so it can be, and is, tested against fixed vectors
// (WebAuthnTester, run by ClassTester on every debug start).
//
// WHAT IS VERIFIED
//
//   clientDataJSON      its type (webauthn.create or webauthn.get), the
//                       challenge it echoes and the origin it was collected at.
//                       The caller compares all three; this only reads them.
//   authenticatorData   the SHA-256 of the relying party id, the flags (user
//                       present, user verified), the signature counter and,
//                       for a registration, the attested credential data: the
//                       authenticator's AAGUID, the credential id and the COSE
//                       public key, whose exact bytes are kept so the key can
//                       be stored and used again.
//   the signature       over authenticatorData || SHA-256(clientDataJSON), with
//                       OpenSSL: ECDSA P-256 with SHA-256 for ES256 (-7), RSA
//                       PKCS#1 v1.5 with SHA-256 for RS256 (-257). No other
//                       algorithm is accepted, so an authenticator that offers
//                       one this server cannot check is refused at registration
//                       rather than trusted at sign-in.
//
// WHAT IS NOT VERIFIED, AND WHY
//
// THE ATTESTATION STATEMENT. A registration carries an attStmt - a statement,
// signed by a key the authenticator's manufacturer certifies, saying what kind
// of device made this credential. Verifying it needs the manufacturer root
// certificates (the FIDO Metadata Service) and a policy about which models are
// acceptable, neither of which this server has or ships. So the attestation
// object is read for its authData alone and the statement is ignored: this
// server learns that a public key was created and can check every signature it
// makes from then on, and it does NOT learn what kind of device holds the
// private key. The registration options ask for attestation "none" for that
// reason - asking for a statement nobody checks would be theatre. The practical
// consequence is that an installation cannot say "only hardware keys of this
// make"; it can say "this credential, on this device, from now on".
//
// The CBOR reader is deliberately small: the definite-length subset RFC 8949
// calls canonical is what an authenticator sends, and an indefinite length, a
// tag or a float is refused rather than skipped, since a document this server
// cannot read exactly is one it must not half-read.
#pragma once

#include <string>

namespace HM
{
   class WebAuthn
   {
   public:

      // base64url without padding (RFC 4648 section 5), both ways. WebAuthn
      // carries every binary value in it - the challenge, the credential id,
      // the user handle - so the page and the server spell them the same.
      static AnsiString Base64Url(const unsigned char *data, size_t length);
      static AnsiString Base64Url(const std::string &bytes);
      static bool Base64UrlDecode(const std::string &text, std::string &bytes);

      static std::string Sha256(const std::string &bytes);

      // The flags byte of authenticator data. UV is what makes a passkey a
      // second factor as well as a first: the authenticator asked the person
      // for a PIN or a fingerprint before it signed.
      static const unsigned char FlagUserPresent = 0x01;
      static const unsigned char FlagUserVerified = 0x04;
      static const unsigned char FlagAttestedData = 0x40;

      struct ClientData
      {
         std::string type;        // webauthn.create, webauthn.get
         std::string challenge;   // base64url, exactly as the browser wrote it
         std::string origin;      // https://mail.example.test
         bool cross_origin = false;
      };

      // Reads clientDataJSON. The caller decides whether the three values are
      // the ones it asked for; nothing is compared here.
      static bool ReadClientData(const std::string &json, ClientData &data, AnsiString &problem);

      struct AuthenticatorData
      {
         std::string rp_id_hash;      // 32 bytes: SHA-256 of the rp id
         unsigned char flags = 0;
         unsigned int sign_count = 0;

         // Present only when FlagAttestedData is set - a registration.
         bool attested = false;
         std::string aaguid;          // 16 bytes
         std::string credential_id;
         std::string public_key;      // the COSE key, exactly its own bytes
      };

      static bool ReadAuthenticatorData(const std::string &bytes, AuthenticatorData &data, AnsiString &problem);

      // The authData and the fmt of a registration's attestationObject. The
      // attStmt is stepped over, not verified; see the header comment.
      static bool ReadAttestationObject(const std::string &bytes, std::string &authenticatorData,
                                        AnsiString &format, AnsiString &problem);

      struct PublicKey
      {
         int algorithm = 0;              // -7 ES256, -257 RS256
         std::string x, y;               // EC P-256, 32 bytes each
         std::string modulus, exponent;  // RSA
      };

      static const int AlgorithmEs256 = -7;
      static const int AlgorithmRs256 = -257;

      // A COSE_Key (RFC 8152) as the attested credential data carries it.
      static bool ReadPublicKey(const std::string &cose, PublicKey &key, AnsiString &problem);

      // The assertion's signature over authenticatorData || SHA-256(clientDataJSON).
      // False for a signature that does not verify, a key this build cannot use,
      // and anything OpenSSL refuses; there is one answer for all of them,
      // because the caller must not be able to tell them apart.
      static bool VerifySignature(const PublicKey &key, const std::string &authenticatorData,
                                  const std::string &clientDataJson, const std::string &signature);
   };

   class WebAuthnTester
   {
   public:
      void Test();
   };
}
