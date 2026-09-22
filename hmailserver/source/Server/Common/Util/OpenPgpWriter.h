// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Writing one kind of OpenPGP message on the server: a message encrypted to a
// public key, and nothing else. This is what a zero-access mailbox
// (docs/ZeroAccess.md) needs at the moment of delivery - the filters have run
// over the text, and the text is about to be written into the mailbox, and it
// is written encrypted to the account's own key instead - and it is the whole
// of what this server can do with OpenPGP: it never signs, never decrypts and
// never verifies. Everything else is the page's (PortalOpenPgp.js).
//
// WHAT IS WRITTEN, packet by packet (RFC 9580, the 2024 refresh of RFC 4880)
//
//   * a Public-Key Encrypted Session Key packet (tag 1, section 5.1), version 3,
//     naming the key id of the (sub)key it is for, carrying a fresh AES-256
//     session key. For an RSA key the session key is under RSAES-PKCS1-v1_5
//     (section 5.1.3, as OpenPGP has always done it); for an ECDH key on
//     Curve25519 - the cv25519 curve GnuPG makes by default - it is under the
//     RFC 6637 scheme: an ephemeral X25519 agreement, the one-step KDF of
//     section 11.5 over the shared point with the recipient's fingerprint in
//     the input, and the RFC 3394 AES key wrap of the padded key material.
//     Both are exactly what GnuPG writes and reads, which is how this is
//     proved: what this writes, gpg --decrypt opens.
//   * a Symmetrically Encrypted and Integrity Protected Data packet (tag 18,
//     section 5.13), version 1: AES-256 in OpenPGP's CFB mode with the random
//     prefix and its two repeated octets, over a Literal Data packet (tag 11,
//     binary, no filename, no date) holding the message, followed by the
//     Modification Detection Code packet - the SHA-1 of everything before it.
//     Version 2 (AEAD) is not written: GnuPG 2.4 does not read it by default,
//     and the page does not read it at all.
//
// Nothing is compressed: the payload is a MIME message that may hold an
// attachment already compressed, and a reader that has to decompress before
// it can check the MDC is a reader given more work by whoever wrote the
// message. Both readers this is proved against read an uncompressed message.
//
// WHICH KEY IS ENCRYPTED TO
//
// ChooseEncryptionKey walks a transferable public key - one OpenPgpKey::Parse
// has already accepted - the way a client does: the newest subkey whose
// binding signature carries the encrypt flags and which is neither revoked nor
// expired, else the primary itself when it is RSA and flagged to encrypt. The
// flags, the expiry and the revocation are read from the signatures' hashed
// subpackets; the signatures are NOT verified cryptographically, for the same
// reason OpenPgpKey.h gives: the key is the account's own, put in the ring by
// the account holder, and verifying its self-signatures would need every
// signature algorithm OpenPGP has without making the key any more theirs.
//
// WHERE THE RANDOMNESS COMES FROM, AND HOW THE TESTS SEE IT
//
// The session key, the CFB prefix and the ephemeral scalar come from
// OpenSSL's RAND_bytes. OpenPgpWriterTester hands in fixed values through the
// Randomness struct, which makes the output a function of its inputs and lets
// the test compare the packets to bytes computed elsewhere - and then
// decrypts the result with the same session key, so that a CFB or MDC mistake
// shows as a mismatch rather than as a message some reader might tolerate.
// The bench proof is docs/ZeroAccess.md's: the Linux build's openpgp-writer
// tool hands what this writes to GnuPG 2.4 on every CI run, and the
// regression fixture does the same on a bench that has gpg.
#pragma once

#include <string>
#include <vector>

namespace HM
{
   class OpenPgpWriter
   {
   public:
      // The (sub)key a message is encrypted to, read out of a transferable
      // public key. ok is the only thing a caller may act on; when it is false,
      // error is a whole sentence and every other field is undefined.
      struct EncryptionKey
      {
         EncryptionKey() : ok(false), algorithm(0), kdf_hash(0), kdf_cipher(0), created(0), expires(0) {}

         bool ok;
         AnsiString error;

         int algorithm;                         // 1 or 2 (RSA), 18 (ECDH on Curve25519)
         AnsiString fingerprint;                // of the (sub)key encrypted to, lower-case hex
         AnsiString primary_fingerprint;        // of the key it belongs to, lower-case hex
         std::vector<unsigned char> fingerprint_bytes;   // 20 octets, the version 4 fingerprint
         std::vector<unsigned char> key_id;              // the last 8 of them

         // RSA: the modulus and the public exponent, big-endian, no MPI header.
         std::vector<unsigned char> rsa_n;
         std::vector<unsigned char> rsa_e;

         // ECDH: the 32-octet Curve25519 point (u coordinate, little-endian, as
         // X25519 takes it - the 0x40 prefix of the MPI stripped), and the KDF
         // parameters the key carries: the hash (8 SHA-256, 9 SHA-384, 10
         // SHA-512) and the cipher the session key is wrapped with (7 AES-128,
         // 8 AES-192, 9 AES-256).
         std::vector<unsigned char> point;
         int kdf_hash;
         int kdf_cipher;

         __int64 created;
         __int64 expires;     // 0 when it does not expire
      };

      // now is the moment expiry is judged at; the caller passes the clock so
      // that the tester can stand still.
      static EncryptionKey ChooseEncryptionKey(const unsigned char *key, size_t length, __int64 now);
      static EncryptionKey ChooseEncryptionKeyBase64(const AnsiString &keyBase64, __int64 now);

      // The test hook. Empty vectors mean "draw fresh ones".
      struct Randomness
      {
         std::vector<unsigned char> session_key;        // 32 octets
         std::vector<unsigned char> prefix;             // 16 octets
         std::vector<unsigned char> ephemeral_scalar;   // 32 octets, ECDH only
      };

      // The OpenPGP message - PKESK then SEIPD - as binary packets.
      static bool Encrypt(const EncryptionKey &to, const unsigned char *data, size_t length,
                          std::vector<unsigned char> &message, AnsiString &error, const Randomness *fixed = nullptr);

      // RFC 9580 section 6: the radix-64 armour with its CRC-24, CRLF lines.
      static AnsiString Armour(const std::vector<unsigned char> &message);

      // RFC 3156: the message as a whole, encrypted. The outer headers are the
      // original's with every Content-* and MIME-Version field taken off,
      // then multipart/encrypted with the two parts the RFC names; what is
      // encrypted is the ENTIRE original message, headers and all, so that
      // the reader sees exactly what arrived (and Thunderbird's protected
      // headers see the inner ones). original is the message's bytes as they
      // are on disk, CRLF lines.
      static bool SealMessage(const EncryptionKey &to, const AnsiString &original,
                              AnsiString &sealed, AnsiString &error, const Randomness *fixed = nullptr);

      // Whether a message on disk is one this wrote, or one shaped like it: a
      // top-level multipart/encrypted with protocol application/pgp-encrypted.
      // Read from the header block only.
      static bool IsPgpMimeEncrypted(const AnsiString &message);

      // The packet header for tag and a body of length octets, new format.
      static void WritePacketHeader(int tag, size_t length, std::vector<unsigned char> &out);

   private:
      static bool EncryptSessionKeyRsa_(const EncryptionKey &to, const std::vector<unsigned char> &material,
                                        std::vector<unsigned char> &body, AnsiString &error);
      static bool EncryptSessionKeyEcdh_(const EncryptionKey &to, const std::vector<unsigned char> &material,
                                         const std::vector<unsigned char> &scalar, std::vector<unsigned char> &body, AnsiString &error);
   };

   // Runs from ClassTester (MainOperations.TestInternals in the suite). Throws
   // std::logic_error naming what was wrong; every case is a value.
   class OpenPgpWriterTester
   {
   public:
      void Test();
   };
}
