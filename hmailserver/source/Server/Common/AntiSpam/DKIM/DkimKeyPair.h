// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

namespace HM
{
   // A DKIM key made and read by the server itself.
   //
   // Until key rotation, the server never generated a DKIM key: the desktop Control
   // Panel did, on its own machine, so a Linux server administered from the Deck, or
   // a Windows server administered from another machine, had no way to make a key
   // the server could read. This is the server's half of that: an RSA key as PEM,
   // and the public half in exactly the form a verifier reads out of the p= tag -
   // SubjectPublicKeyInfo for RSA, which is what DKIM::VerifyHeaderHash_ hands to
   // d2i_PUBKEY, and the raw 32 bytes for Ed25519 (RFC 8463). Generation is RSA only:
   // switching a domain to Ed25519 alone loses DKIM at every verifier that has not
   // implemented it, which is still most of them; a hand-made Ed25519 key is read
   // and checked like any other.
   class DkimKeyPair
   {
   public:
      // 2048, 3072 or 4096 bits. False, with the reason in error, when OpenSSL
      // refuses - which is a fault, and the caller reports it.
      static bool Generate(int bits, AnsiString &pem, String &error);

      // The key type word the k= tag carries ("rsa" or "ed25519") and the public key
      // bytes p= carries, from a private key in PEM. False when the text is not a
      // private key this server can sign with.
      static bool Derive(const AnsiString &pem, AnsiString &keyType, std::vector<unsigned char> &publicKey);

      // Derive, from a file. False when the file cannot be read or does not hold a
      // usable private key; error then says which.
      static bool DeriveFromFile(const String &path, AnsiString &keyType, std::vector<unsigned char> &publicKey, String &error);

      // The TXT record to publish: "v=DKIM1; k=rsa; p=<base64>". No h= (every
      // signature this server makes is SHA-256) and no t= (a key in testing mode
      // tells verifiers to ignore failures, which is not what a rotation wants).
      static AnsiString RecordValue(const AnsiString &keyType, const std::vector<unsigned char> &publicKey);
   };
}
