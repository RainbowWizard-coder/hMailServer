// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Reading an OpenPGP transferable public key far enough to be sure of what it
// is, and the one piece of arithmetic a Web Key Directory needs.
//
// WHY THIS IS A UNIT OF ITS OWN
//
// This server publishes its users' public keys and holds nothing else: no
// secret key, ever. That promise is only as good as the thing that decides
// what an upload is, so the deciding is here, in one small file with no
// database, no socket, no clock and no settings behind it - every input is a
// parameter - and OpenPgpKeyTester walks it in process from ClassTester. Every
// later OpenPGP row leans on this, so it is written to be read.
//
// WHAT IS PARSED, AND WHAT IS NOT
//
// Parse() walks the packet framing of RFC 9580 (the 2024 refresh of RFC 4880):
// old-format and new-format headers, one-, two-, four- and five-octet lengths.
// It requires the shape of a transferable public key from section 10.1 - a
// primary public-key packet, then certifications, user ids, user attributes and
// subkeys, each followed by its signatures - and it requires the packets to add
// up: the last one must end exactly at the last byte, with nothing after it and
// nothing missing from it.
//
// It REFUSES, each with a sentence a person can act on:
//
//   * a secret-key packet (tag 5) or a secret-subkey packet (tag 7), anywhere,
//     and an ASCII-armoured PRIVATE KEY BLOCK before a byte of it is decoded;
//   * a packet that is not part of a public key - a message, a marker, a trust
//     packet that belongs only to a local keyring;
//   * a second primary key: one upload is one key;
//   * a partial or indeterminate body length, which frames a stream and not a key;
//   * a version 3 key, whose fingerprint is not a hash of the key material at all;
//   * a key packet or subkey packet with bytes after its public key material -
//     which is what a secret-key packet becomes when its one tag byte is
//     relabelled as a public one. A version 5 or 6 packet counts its material;
//     a version 4 packet is walked field by field for every algorithm RFC 9580
//     names, and only a version 4 packet of an algorithm outside that list
//     cannot be checked and is taken at its word;
//   * a key with no user id, or with no self-signature;
//   * anything past the size cap the caller passes (the server's is
//     OpenPGPKeyMaximumBytes, 64 KiB by default - see docs/OpenPGP.md).
//
// It does NOT verify a signature cryptographically. "Self-signature" here means
// a certification signature whose issuer fingerprint or issuer key id is this
// key's own - the packet says it was made by this key. Verifying it would mean
// implementing every signature algorithm OpenPGP has, and would still not make
// the key trustworthy: what makes a key in this directory the right one is that
// an administrator or the account holder put it there. docs/OpenPGP.md says so
// in those words, so that nobody reads more into a stored key than is there.
//
// THE HASHED LOCAL PART
//
// HashedLocalPart is draft-koch-openpgp-webkey-service section 3.1: the local
// part with its ASCII letters lower-cased and every other octet untouched,
// SHA-1, z-base-32 with the alphabet ybndrfg8ejkmcpqxot1uwisza345h769. The
// draft's own example is a test: Joe.Doe -> iy9q119eutrkn8s1mk4r39qejnbu3n5q.

#pragma once

#include <string>
#include <vector>

namespace HM
{
   class OpenPgpKey
   {
   public:
      // The cap Parse applies when the caller passes 0, and the default of the
      // OpenPGPKeyMaximumBytes setting. A key with a few hundred third-party
      // certifications on it is tens of kilobytes; one that is megabytes is
      // either a keyring or an attack on whoever fetches it.
      static const size_t DefaultMaximumBytes = 65536;

      // The public-key algorithm numbers of RFC 9580 section 9.1 that this
      // server has a name for. An unknown number is kept as a number: a key
      // this server cannot name is still a key it can publish.
      enum Algorithm
      {
         AlgorithmRsa = 1,
         AlgorithmRsaEncryptOnly = 2,
         AlgorithmRsaSignOnly = 3,
         AlgorithmElgamal = 16,
         AlgorithmDsa = 17,
         AlgorithmEcdh = 18,
         AlgorithmEcdsa = 19,
         AlgorithmEddsaLegacy = 22,
         AlgorithmX25519 = 25,
         AlgorithmX448 = 26,
         AlgorithmEd25519 = 27,
         AlgorithmEd448 = 28
      };

      // What Parse made of the bytes. ok is the only thing a caller may act on;
      // when it is false, error is a whole sentence and every other field is
      // undefined.
      struct Parsed
      {
         Parsed()
            : ok(false), version(0), algorithm(0), bits(0), created(0), expires(0),
              user_id_count(0), subkey_count(0), signature_count(0), size(0)
         {
         }

         bool ok;
         AnsiString error;

         // Lower-case hex: 40 characters for a version 4 key (SHA-1 of the key
         // material) and 64 for a version 6 one (SHA-256), as RFC 9580 section
         // 5.5.4 defines them.
         AnsiString fingerprint;
         AnsiString key_id;            // 16 hex characters, for a person reading a log

         int version;                  // 4, 5 or 6
         int algorithm;                // RFC 9580 section 9.1
         AnsiString algorithm_name;    // "RSA", "Ed25519", "algorithm 99"
         int bits;                     // the modulus or prime size; 0 for a named curve
         __int64 created;              // seconds since the epoch, from the key packet
         __int64 expires;              // 0 when the key does not expire

         AnsiString primary_user_id;
         std::vector<AnsiString> user_ids;

         int user_id_count;
         int subkey_count;
         int signature_count;

         // The transferable public key itself, binary, exactly the bytes that
         // were parsed. This is what a Web Key Directory serves and what the
         // store keeps; nothing is re-serialised, because a key this server
         // cannot re-sign is a key it must not rewrite.
         std::vector<unsigned char> bytes;
         size_t size;
      };

      // The binary form. maximum of 0 means DefaultMaximumBytes.
      static Parsed Parse(const unsigned char *data, size_t length, size_t maximum);

      // What arrives over REST, over COM and from a command line: an
      // ASCII-armoured block, or base64 of the binary form. A PRIVATE KEY BLOCK
      // is refused here, by its armour header, before anything is decoded.
      static Parsed ParseText(const AnsiString &text, size_t maximum);

      // RFC 9580 section 6: the radix-64 armour, its header line and its CRC-24
      // checksum. False with a sentence in error when the block is not one.
      static bool Dearmour(const AnsiString &text, std::vector<unsigned char> &out, AnsiString &error);

      // The same radix-64 reading for a PGP MESSAGE block - what OpenPgpWriter
      // armours - with the checksum checked when it is there. This server
      // never decrypts one; the writer's self-test reads its own armour back.
      static bool DearmourMessage(const AnsiString &text, std::vector<unsigned char> &out, AnsiString &error);

      // The CRC-24 of section 6.1 over the bytes, for whoever writes armour
      // (OpenPgpWriter); Dearmour checks the same sum on the way in.
      static unsigned int ArmourChecksum(const unsigned char *data, size_t length);

      // What ReadSecretKey made of an armoured PRIVATE KEY BLOCK. ok is the
      // only thing a caller may act on; when it is false, error is a whole
      // sentence and everything else is undefined.
      struct SecretKeyFacts
      {
         SecretKeyFacts() : ok(false), version(0), algorithm(0), secret_packets(0), unprotected_packets(0) {}

         bool ok;
         AnsiString error;
         AnsiString fingerprint;      // of the primary key, lower-case hex
         AnsiString key_id;
         AnsiString primary_user_id;
         int version;
         int algorithm;
         int secret_packets;          // the primary and its subkeys
         int unprotected_packets;     // of those, the ones in the clear
      };

      // A SECRET key, read for one question: is every secret packet in it
      // encrypted under a passphrase? The webmail's key store holds a person's
      // secret key in the OpenPGP format so that the same bytes open in GnuPG,
      // and the encryption is done in the browser where this server cannot see
      // it - so this is the one place that can refuse a key that arrived
      // without it. Nothing here decrypts anything and no passphrase is
      // involved. The Web Key Directory's own door (Dearmour, ParseText) still
      // refuses a secret key outright; this is a different door, for the one
      // caller that is meant to hold one.
      static SecretKeyFacts ReadSecretKey(const AnsiString &text, size_t maximum);

      // draft-koch-openpgp-webkey-service section 3.1. localPart is the part
      // before the @, in UTF-8; the answer is 32 characters of z-base-32.
      static AnsiString HashedLocalPart(const AnsiString &localPart);

      // The z-base-32 of the draft's section 3.1 - five bits a character, most
      // significant first, no padding.
      static AnsiString ZBase32(const unsigned char *data, size_t length);

      // True when one of the key's user ids carries this address. The
      // comparison is on the address inside the angle brackets when there are
      // any, on the whole user id when there are none, and is case-insensitive:
      // a mail address's domain always is, and a local part that differs only
      // in case is the same mailbox on this server.
      static bool CarriesAddress(const Parsed &key, const AnsiString &address);

      // The address a user id names: "Joe Doe <joe.doe@example.org>" ->
      // "joe.doe@example.org", lower-cased. Empty when there is none.
      static AnsiString AddressOfUserId(const AnsiString &userId);

   private:
      struct Packet
      {
         Packet() : tag(0), body_start(0), body_length(0), next(0) {}
         int tag;
         size_t body_start;
         size_t body_length;
         size_t next;
      };

      static bool ReadArmour_(const std::string &source, const std::string &label, std::vector<unsigned char> &out, AnsiString &error);
      static bool ReadPacket_(const unsigned char *data, size_t length, size_t offset, Packet &packet, AnsiString &error);
      static bool ReadKeyPacket_(const unsigned char *data, const Packet &packet, Parsed &parsed, AnsiString &error);
      static bool ReadSubkeyPacket_(const unsigned char *data, const Packet &packet, AnsiString &error);
      static bool PublicMaterialEnd_(const unsigned char *body, size_t length, int version, int algorithm,
                                     size_t &material_at, size_t &material_end, AnsiString &error);
      static void ReadSignature_(const unsigned char *data, const Packet &packet, const Parsed &key,
                                 bool &is_self_certification, bool &is_primary_flag, __int64 &expiry_seconds);
      static AnsiString HexOf_(const unsigned char *data, size_t length);
      static AnsiString NameOfAlgorithm_(int algorithm);
   };

   // Runs from ClassTester (MainOperations.TestInternals in the suite, and
   // hMailServer --test-internals on a console). Throws std::logic_error naming
   // what was wrong; every case is a value, so nothing here needs a bench.
   class OpenPgpKeyTester
   {
   public:
      void Test();
   };
}
