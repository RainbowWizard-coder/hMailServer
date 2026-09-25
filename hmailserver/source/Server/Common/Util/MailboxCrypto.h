// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The cryptography of mailbox encryption at rest (docs/EncryptionAtRest.md),
// as functions of values: no database, no settings, no state. MailboxKeys.h
// is what keeps the keys; MessageStoreFormat is what puts a message through
// them. Everything here is OpenSSL's, called the way OpenPgpWriter and
// DataProtector already call it.
//
// THE PIECES
//
//   A key pair         X25519 (RFC 7748): 32 raw bytes private, 32 public. One
//                      per account (the mailbox key) and one per domain (the
//                      recovery key).
//
//   Sealed to a key    what a stored message and the recovery copy of a mailbox
//                      key are: an ephemeral X25519 pair made for this one
//                      seal, its agreement with the recipient's public key put
//                      through HKDF-SHA256 (RFC 5869; the salt is the two
//                      public keys, the info names the purpose), and the
//                      result the AES-256-GCM key for the bytes, with a random
//                      12-byte nonce and associated data that binds the
//                      recipient and the purpose. Sealing needs only the
//                      recipient's PUBLIC key, which is how mail is encrypted
//                      when nobody is signed in; opening needs the private one.
//
//   Wrapped by a secret what the password copy of a mailbox key and the
//                      recovery key's private half are: a key-encryption key
//                      derived from the secret with Argon2id (RFC 9106, the
//                      server's own password-hash parameters, never below
//                      19 MiB and two passes) - or, on a build whose OpenSSL
//                      has no Argon2 (3.0, 3.1), PBKDF2-HMAC-SHA256 at 600,000
//                      iterations - and AES-256-GCM under it.
//
// THE TEXT FORMS, stored in the database and never parsed from anywhere else:
//
//   hmk1$a2$<memory KiB>$<passes>$<lanes>$<salt>$<nonce>$<ciphertext+tag>
//   hmk1$p2$<iterations>$<salt>$<nonce>$<ciphertext+tag>
//   hmk1$x$<ephemeral public key>$<nonce>$<ciphertext+tag>
//
// each field base64 or a decimal number. A form this build does not know, or
// parameters outside the bounds below, is refused rather than tried: a row
// that asked for a trivial derivation, or one that would take a gigabyte and
// a minute, is damage or an attack, not a key.
#pragma once

#include <functional>
#include <string>

namespace HM
{
   class MailboxCrypto
   {
   public:
      enum Sizes
      {
         KeyBytes = 32,
         NonceBytes = 12,
         TagBytes = 16,
         SaltBytes = 16,

         // The parameters of a message's encryption stage in its container:
         // the version (1), the recipient's public key, the ephemeral public
         // key, the nonce and the tag.
         StageParameterBytes = 1 + 32 + 32 + 12 + 16,
         StageVersion = 1
      };

      // Bounds on a stored derivation, for the reason the header gives.
      static const int MinimumMemoryKiB = 19456;
      static const int MaximumMemoryKiB = 1048576;
      static const int MinimumPasses = 2;
      static const int MaximumPasses = 10;
      static const int MaximumLanes = 16;
      static const int Pbkdf2Iterations = 600000;

      // A new key pair. False only when OpenSSL cannot make one.
      static bool GenerateKeyPair(std::string &privateKey, std::string &publicKey);

      // The public key of a private one; empty when it is not a key.
      static std::string PublicKeyOf(const std::string &privateKey);

      // Random bytes from OpenSSL's generator; empty on failure.
      static std::string RandomBytes(size_t count);

      // ---- sealed to a public key ------------------------------------------------

      // The text form above, of plain sealed to recipientPublic. purpose goes
      // into the HKDF info and the associated data, so a copy sealed for one
      // purpose does not open as another.
      static bool SealToPublic(const std::string &recipientPublic, const std::string &purpose, const std::string &plain,
                               std::string &sealed, String &problem);
      static bool OpenSealed(const std::string &recipientPrivate, const std::string &purpose, const std::string &sealed,
                             std::string &plain, String &problem);

      // ---- wrapped by a secret ---------------------------------------------------

      // purpose is the associated data: a wrap made for one account's key does
      // not open as another's, since the public key is in it.
      static bool WrapWithSecret(const AnsiString &secret, const std::string &purpose, const std::string &plain,
                                 std::string &wrapped, String &problem);

      // False when the secret is wrong, the text is not a wrap this build
      // reads, or it has been changed; problem says which, and never anything
      // about the secret itself.
      static bool UnwrapWithSecret(const AnsiString &secret, const std::string &purpose, const std::string &wrapped,
                                   std::string &plain, String &problem);

      // ---- a message's encryption stage ------------------------------------------

      // Encrypts the body of a container to the recipient's public key.
      // context is what the container's header says of the message before
      // this stage (its size and the stages under this one); it and both
      // public keys are the associated data. parameters is the stage's
      // StageParameterBytes, cipher the encrypted body, the same length as
      // the plain one. The bytes are decrypted again in memory before this
      // answers true, so what is returned is known to open.
      static bool SealStage(const std::string &recipientPublic, const std::string &context, const std::string &plain,
                            std::string &parameters, std::string &cipher, String &problem);

      // The private key for a recipient's public key, or false with the
      // reason (MailboxKeys: the key is not open).
      typedef std::function<bool(const std::string &recipientPublic, std::string &privateKey, String &problem)> KeyFinder;

      // The recipient's public key a stage's parameters name, or empty when
      // they are not a stage's.
      static std::string StageRecipient(const std::string &parameters);

      static bool OpenStage(const std::string &parameters, const std::string &context, const std::string &cipher,
                            const KeyFinder &keys, std::string &plain, String &problem);

      // ---- plumbing ------------------------------------------------------------

      static std::string Base64(const std::string &bytes);
      static bool FromBase64(const std::string &text, std::string &bytes);

      // SHA-256 of the bytes as 64 lower-case hex digits, and its first 16:
      // the fingerprint a key is shown by.
      static AnsiString Fingerprint(const std::string &publicKey);

      // Written over with zeros, then emptied: what is done with every
      // private key and derived key this code holds, as soon as it is done.
      static void Forget(std::string &secret);

      static bool ConstantTimeEquals(const std::string &one, const std::string &two);

      // HKDF-SHA256 (RFC 5869), extract and expand, for at most 32 bytes.
      static std::string Hkdf(const std::string &inputKey, const std::string &salt, const std::string &info, size_t length);

      // X25519 agreement; empty on failure.
      static std::string Agree(const std::string &privateKey, const std::string &peerPublic);

      // AES-256-GCM, both ways.
      static bool Encrypt(const std::string &key, const std::string &nonce, const std::string &aad, const std::string &plain,
                          std::string &cipher, std::string &tag);
      static bool Decrypt(const std::string &key, const std::string &nonce, const std::string &aad, const std::string &cipher,
                          const std::string &tag, std::string &plain);

      // Whether Argon2id is what a new wrap is made with on this build.
      static bool Argon2Available();

      // Known answers (RFC 7748's X25519 vector, RFC 5869's first HKDF case),
      // and every construction above round-tripped with its refusals: a wrong
      // secret, a changed byte, a wrong key, a copy made for another purpose.
      static void Test();
   };
}
