// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "LegacyBlowfish.h"

namespace HM
{
   class Crypt : public Singleton<Crypt>
   {
   public:
      Crypt();
      virtual ~Crypt();

      enum EncryptionType
      {
         ETNone = 0,
         ETBlowFish = 1,
         ETMD5 = 2,
         ETSHA256 = 3,
         ETPBKDF2 = 4,
         ETArgon2id = 5,
         ETDPAPI = 6,
         // scrypt (RFC 7914), the third strong KDF. 7 rather than 6 because DPAPI took
         // 6 first, and the numbers are what the ini and the database store.
         ETScrypt = 7
      };

      EncryptionType GetHashType(const String &hash);

      // Where a scheme stands when one is compared with another: the enum values are
      // storage identifiers, not an order. Argon2id and scrypt are peers - both
      // memory-hard, neither an upgrade of the other - so a preference for one
      // leaves accounts stored under the other alone, and a minimum of either
      // accepts both. DPAPI is not a password scheme and ranks with "none".
      static int StrengthRank(int encryptionType);

      String EnCrypt(const String &sInput, EncryptionType iType) const;
      String DeCrypt(const String &sInput, EncryptionType iType) const;

      bool Validate(const String &password, const String &originalHash, EncryptionType iType) const;

      // What a password check found beyond yes or no.
      struct PasswordMatch
      {
         PasswordMatch() : refused_old_conversion(false), legacy_bytes(false) { }

         // On a match, the text a re-hash is to be made from: the password, read
         // as UTF-8 when it arrived as its UTF-8 widened one byte at a time (an
         // IMAP LOGIN, a REST Basic credential). Either form hashes to the same
         // bytes (Unicode::ToUtf8Canonical); a Latin-1 password that matched a
         // hash stored before 6.3.4 is re-hashed over its UTF-8.
         String text;

         // A match, but only as the bytes the conversion before 6.3.4 made: a
         // Latin-1 password's characters as bytes, or the password peppered
         // under the pepper as that conversion keyed it. The caller re-hashes
         // from text, so that the stored hash is over UTF-8 - which is what an
         // IMAP LOGIN or a REST Basic credential of the same password, sent as
         // UTF-8, and a SCRAM-SHA-256 client derive from.
         bool legacy_bytes;

         // No match, but the stored hash is the one the conversion before 6.3.4
         // made of this password, which kept nothing of a character above
         // U+00FF but the length (a NUL byte for it and every byte after it on
         // Windows, a '?' for it on Linux). Refused: another password of that
         // form would have matched too. The password is to be set again.
         bool refused_old_conversion;
      };

      // Validate, saying which text matched and whether the stored hash is one
      // the old conversion made. A hash is checked over the password's UTF-8
      // (Unicode::ToUtf8Canonical: a password that arrived as its UTF-8 widened
      // byte by byte is those bytes), then - for Latin-1 text, every character
      // below U+0100 and the bytes not UTF-8 - over its characters as bytes,
      // which is what the old conversion made of it exactly. A password holding
      // U+0000 is refused.
      bool Validate(const String &password, const String &originalHash, EncryptionType iType, PasswordMatch *match) const;

      // Protects a reversible secret for storage (route/fetch/relayer passwords).
      // When protection is enabled (the default, ProtectStoredSecretsWithDPAPI=1)
      // the result is a self-describing envelope: machine-bound "DPAPI:<base64>"
      // on Windows, key-file-bound "LINUX1:<base64>" on Linux (see DataProtector).
      // On Windows a DPAPI failure falls back to the legacy Blowfish form so a
      // secret is never lost; on Linux it does not - the result is empty, an error
      // says so, and the caller must treat empty as "not stored". Empty input
      // returns empty on both.
      String ProtectSecret(const String &sInput) const;

      // Reverses ProtectSecret. A prefixed value goes to the store its prefix names
      // when that store is this platform's, and is reported once and answered with
      // an empty string when it is not (a database moved between Windows and
      // Linux); any other (legacy) value is decrypted with Blowfish, so existing
      // stored secrets keep working transparently. Empty input returns empty.
      String UnprotectSecret(const String &sStored) const;

      // Whether a stored value is one of ProtectSecret's machine envelopes,
      // "DPAPI:" or "LINUX1:" - a value only the platform's own store opens.
      static bool IsSecretEnvelope(const String &stored);

   private:

      // The one report for an envelope from the other platform, whichever
      // platform this is: a moved database holds one such value per route, fetch
      // account and relaying domain, and the first says everything the rest would.
      static void ReportForeignEnvelopeOnce_(int errorId, const String &message);

      // Applies the optional server-wide password pepper (HMAC-SHA256 under the
      // configured secret) ahead of Argon2id and scrypt hashing/verification. Returns the
      // password unchanged when no pepper is configured. The pepper is keyed as its
      // UTF-8; oldConversion keys it as it was before 6.3.4 (AnsiString of it), which
      // Validate tries too for a pepper outside ASCII so that hashes made then open.
      AnsiString ApplyPepper_(const AnsiString &password, bool oldConversion = false) const;

      // One check of a stored hash against the given bytes. oldPepper, when given,
      // is set when an Argon2id or scrypt hash matched only under the pepper keyed
      // as it was before 6.3.4.
      bool ValidateBytes_(const AnsiString &password, const AnsiString &originalHash, EncryptionType iType, bool *oldPepper = nullptr) const;

      // The legacy Blowfish scheme (ETBlowFish), keyed once; its operations are
      // const, so every thread shares it.
      const LegacyBlowfish legacy_blowfish_;

   };
}
