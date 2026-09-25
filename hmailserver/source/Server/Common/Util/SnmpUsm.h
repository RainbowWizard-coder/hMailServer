// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cryptography of the SNMPv3 User-based Security Model as this agent speaks
// it, and nothing else: one authentication protocol and one privacy protocol.
//
//   authentication  usmHMAC192SHA256AuthProtocol (RFC 7860): HMAC-SHA-256 over
//                   the whole message with the 24 octets of
//                   msgAuthenticationParameters zeroed, truncated to 24 octets.
//   privacy         usmAesCfb128Protocol (RFC 3826): AES-128 in CFB mode with a
//                   128-bit feedback, no padding; the IV is the authoritative
//                   engine's boots and time, four octets each, and the 64-bit
//                   salt the sender puts in msgPrivacyParameters.
//   keys            RFC 3414 A.2's password-to-key algorithm with SHA-256 in
//                   place of SHA-1, as RFC 7860 section 9.3 says: one megabyte
//                   of the password repeated, hashed; then localised to the
//                   engine as H(Ku || engineID || Ku). The privacy key is the
//                   first 16 octets of the key localised with the
//                   authentication protocol's hash (RFC 3826 section 1.2).
//
// Every function answers false rather than throwing, and OpenSSL is the only
// implementation of any primitive here - the tree links it, and a second AES
// would be a second thing to get wrong.

#pragma once

#include "SnmpBer.h"

#include <string>

namespace HM
{
   class SnmpUsm
   {
   public:
      static const size_t KeyLength = 32;          // a localised SHA-256 key
      static const size_t AuthParameterLength = 24; // HMAC-SHA-256 truncated to 192 bits
      static const size_t PrivKeyLength = 16;      // AES-128
      static const size_t PrivParameterLength = 8; // the salt

      // The shortest password RFC 3414 section 11.2 will turn into a key.
      static const size_t MinimumPasswordLength = 8;

      // Ku: the password turned into a key. False for a password shorter than
      // MinimumPasswordLength.
      static bool PasswordToKey(const std::string &password, SnmpBytes &key);

      // Kul: Ku localised to one engine.
      static bool LocaliseKey(const SnmpBytes &key, const SnmpBytes &engineId, SnmpBytes &localised);

      // Both at once, for a password and an engine.
      static bool LocalisedKey(const std::string &password, const SnmpBytes &engineId, SnmpBytes &localised);

      // The first 24 octets of HMAC-SHA-256 over the message.
      static bool Digest(const SnmpBytes &authKey, const unsigned char *message, size_t length,
                         unsigned char digest[AuthParameterLength]);

      // Equal in time that does not depend on where they differ.
      static bool DigestsEqual(const unsigned char *left, const unsigned char *right, size_t length);

      // The IV of RFC 3826 section 3.1.2.1.
      static void MakeIv(unsigned int boots, unsigned int engineTime, const unsigned char salt[PrivParameterLength],
                         unsigned char iv[16]);

      // AES-128-CFB128 with the first 16 octets of privKey.
      static bool Encrypt(const SnmpBytes &privKey, const unsigned char iv[16],
                          const unsigned char *data, size_t length, SnmpBytes &out);
      static bool Decrypt(const SnmpBytes &privKey, const unsigned char iv[16],
                          const unsigned char *data, size_t length, SnmpBytes &out);

      static bool Sha256(const unsigned char *data, size_t length, SnmpBytes &digest);

      // Overwrites a key before it is released.
      static void Scrub(SnmpBytes &bytes);
      static void Scrub(std::string &text);
   };
}
