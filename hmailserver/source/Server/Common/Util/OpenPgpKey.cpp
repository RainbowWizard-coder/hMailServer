// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OpenPgpKey.h"
#include "OpenPgpRing.h"

#include <openssl/sha.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The packet tags of RFC 9580 section 5 that a transferable public key is
      // made of, and the two that mean somebody sent a secret key.
      const int TagSignature = 2;
      const int TagSecretKey = 5;
      const int TagPublicKey = 6;
      const int TagSecretSubkey = 7;
      const int TagTrust = 12;
      const int TagUserId = 13;
      const int TagPublicSubkey = 14;
      const int TagUserAttribute = 17;
      const int TagPadding = 21;

      unsigned int ReadBigEndian32(const unsigned char *data)
      {
         return ((unsigned int) data[0] << 24) | ((unsigned int) data[1] << 16) |
                ((unsigned int) data[2] << 8) | (unsigned int) data[3];
      }

      unsigned int ReadBigEndian16(const unsigned char *data)
      {
         return ((unsigned int) data[0] << 8) | (unsigned int) data[1];
      }

      // The subpacket area of a version 4 or version 6 signature. Answers false
      // at the first thing that does not fit, which ends the walk without
      // reading past the packet.
      bool NextSubpacket(const unsigned char *data, size_t end, size_t &at, int &type,
                         const unsigned char *&value, size_t &value_length)
      {
         if (at >= end)
            return false;

         size_t length = 0;
         unsigned char first = data[at];
         if (first < 192)
         {
            length = first;
            at += 1;
         }
         else if (first < 255)
         {
            if (at + 2 > end)
               return false;
            length = (((size_t) first - 192) << 8) + (size_t) data[at + 1] + 192;
            at += 2;
         }
         else
         {
            if (at + 5 > end)
               return false;
            length = ReadBigEndian32(data + at + 1);
            at += 5;
         }

         if (length == 0 || at + length > end)
            return false;

         type = data[at] & 0x7F;
         value = data + at + 1;
         value_length = length - 1;
         at += length;
         return true;
      }

      // RFC 9580 section 6.1: the CRC-24 that ends an armoured block.
      unsigned int Crc24(const unsigned char *data, size_t length)
      {
         unsigned int crc = 0x00B704CEu;
         for (size_t i = 0; i < length; i++)
         {
            crc ^= ((unsigned int) data[i]) << 16;
            for (int bit = 0; bit < 8; bit++)
            {
               crc <<= 1;
               if (crc & 0x01000000u)
                  crc ^= 0x01864CFBu;
            }
         }
         return crc & 0x00FFFFFFu;
      }

      int Radix64Value(char c)
      {
         if (c >= 'A' && c <= 'Z') return c - 'A';
         if (c >= 'a' && c <= 'z') return c - 'a' + 26;
         if (c >= '0' && c <= '9') return c - '0' + 52;
         if (c == '+') return 62;
         if (c == '/') return 63;
         return -1;
      }

      // The radix-64 of the armour, and of a base64 upload. Answers false on any
      // character that is not base64 and on a length that is not a whole number
      // of quanta.
      bool DecodeRadix64(const std::string &text, std::vector<unsigned char> &out)
      {
         out.clear();

         // At most fourteen bits are live in the accumulator - up to seven
         // left over from the last octet, and six more from this symbol - so
         // it is masked to fourteen after every shift. Unsigned, because a
         // signed value shifted past its top bit is undefined behaviour.
         unsigned int accumulator = 0;
         int bits = 0;
         size_t padding = 0;
         size_t symbols = 0;

         for (size_t i = 0; i < text.size(); i++)
         {
            char c = text[i];
            if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
               continue;
            if (c == '=')
            {
               padding++;
               symbols++;
               continue;
            }
            if (padding > 0)
               return false;

            int value = Radix64Value(c);
            if (value < 0)
               return false;

            symbols++;
            accumulator = ((accumulator << 6) | (unsigned int) value) & 0x3FFFu;
            bits += 6;
            if (bits >= 8)
            {
               bits -= 8;
               out.push_back((unsigned char) ((accumulator >> bits) & 0xFF));
            }
         }

         if (symbols % 4 != 0 || padding > 2)
            return false;

         return true;
      }
   }

   bool
   OpenPgpKey::ReadPacket_(const unsigned char *data, size_t length, size_t offset, Packet &packet, AnsiString &error)
   {
      if (offset >= length)
      {
         error = "The key ends in the middle of a packet.";
         return false;
      }

      unsigned char first = data[offset];
      if ((first & 0x80) == 0)
      {
         error = "These bytes are not OpenPGP packets: the first octet of a packet always has its top bit set, and this one does not.";
         return false;
      }

      size_t body_length = 0;
      size_t body_start = 0;

      if (first & 0x40)
      {
         // New format (RFC 9580 section 4.2.1).
         packet.tag = first & 0x3F;

         if (offset + 2 > length)
         {
            error = "The key ends in the middle of a packet header.";
            return false;
         }

         unsigned char o1 = data[offset + 1];
         if (o1 < 192)
         {
            body_length = o1;
            body_start = offset + 2;
         }
         else if (o1 < 224)
         {
            if (offset + 3 > length)
            {
               error = "The key ends in the middle of a packet header.";
               return false;
            }
            body_length = (((size_t) o1 - 192) << 8) + (size_t) data[offset + 2] + 192;
            body_start = offset + 3;
         }
         else if (o1 == 255)
         {
            if (offset + 6 > length)
            {
               error = "The key ends in the middle of a packet header.";
               return false;
            }
            body_length = ReadBigEndian32(data + offset + 2);
            body_start = offset + 6;
         }
         else
         {
            error = "A packet in this key has a partial body length, which frames a stream of data and never a key.";
            return false;
         }
      }
      else
      {
         // Old format (RFC 9580 section 4.2.1, the legacy header).
         packet.tag = (first >> 2) & 0x0F;
         int length_type = first & 0x03;

         if (length_type == 0)
         {
            if (offset + 2 > length)
            {
               error = "The key ends in the middle of a packet header.";
               return false;
            }
            body_length = data[offset + 1];
            body_start = offset + 2;
         }
         else if (length_type == 1)
         {
            if (offset + 3 > length)
            {
               error = "The key ends in the middle of a packet header.";
               return false;
            }
            body_length = ReadBigEndian16(data + offset + 1);
            body_start = offset + 3;
         }
         else if (length_type == 2)
         {
            if (offset + 5 > length)
            {
               error = "The key ends in the middle of a packet header.";
               return false;
            }
            body_length = ReadBigEndian32(data + offset + 1);
            body_start = offset + 5;
         }
         else
         {
            error = "A packet in this key has an indeterminate length, which frames a stream of data and never a key.";
            return false;
         }
      }

      if (body_start > length || body_length > length - body_start)
      {
         error = "A packet in this key says it is longer than the bytes that were sent, so the key is truncated.";
         return false;
      }

      packet.body_start = body_start;
      packet.body_length = body_length;
      packet.next = body_start + body_length;
      return true;
   }

   AnsiString
   OpenPgpKey::HexOf_(const unsigned char *data, size_t length)
   {
      static const char digits[] = "0123456789abcdef";

      AnsiString hex;
      hex.reserve(length * 2);
      for (size_t i = 0; i < length; i++)
      {
         hex += digits[(data[i] >> 4) & 0x0F];
         hex += digits[data[i] & 0x0F];
      }
      return hex;
   }

   AnsiString
   OpenPgpKey::NameOfAlgorithm_(int algorithm)
   {
      switch (algorithm)
      {
      case AlgorithmRsa:            return "RSA";
      case AlgorithmRsaEncryptOnly: return "RSA (encrypt only)";
      case AlgorithmRsaSignOnly:    return "RSA (sign only)";
      case AlgorithmElgamal:        return "ElGamal";
      case AlgorithmDsa:            return "DSA";
      case AlgorithmEcdh:           return "ECDH";
      case AlgorithmEcdsa:          return "ECDSA";
      case AlgorithmEddsaLegacy:    return "EdDSA";
      case AlgorithmX25519:         return "X25519";
      case AlgorithmX448:           return "X448";
      case AlgorithmEd25519:        return "Ed25519";
      case AlgorithmEd448:          return "Ed448";
      }

      AnsiString name;
      name.Format("algorithm %d", algorithm);
      return name;
   }

   // The primary public-key packet: its version, when it was made, what it is,
   // and the fingerprint RFC 9580 section 5.5.4 defines for that version.
   bool
   OpenPgpKey::ReadKeyPacket_(const unsigned char *data, const Packet &packet, Parsed &parsed, AnsiString &error)
   {
      const unsigned char *body = data + packet.body_start;
      size_t length = packet.body_length;

      if (length < 6)
      {
         error = "The key's first packet is too short to be a public key.";
         return false;
      }

      int version = body[0];
      if (version == 2 || version == 3)
      {
         error = "This is a version 3 key. Its fingerprint is a hash of the key's numbers rather than of the key itself, it cannot be published in a Web Key Directory, and no client made since 1998 needs one.";
         return false;
      }
      if (version != 4 && version != 5 && version != 6)
      {
         error.Format("This key says it is version %d, and this server reads versions 4, 5 and 6.", version);
         return false;
      }

      parsed.version = version;
      parsed.created = (__int64) ReadBigEndian32(body + 1);
      parsed.algorithm = body[5];
      parsed.algorithm_name = NameOfAlgorithm_(parsed.algorithm);

      size_t material_at = 6;
      size_t material_end = 0;
      if (!PublicMaterialEnd_(body, length, version, parsed.algorithm, material_at, material_end, error))
         return false;

      if (material_end != length)
      {
         error = "The key's public-key packet carries bytes after its public key material. A public-key packet ends where its key material does; a secret-key packet whose tag has been changed to a public one does not, and this server never holds one.";
         return false;
      }

      // The size of the key, where the algorithm has one to read: the first MPI
      // of RSA, DSA and ElGamal is the modulus or the prime, and an MPI opens
      // with its own bit count. A named curve has no such number, and 0 is the
      // honest answer for it.
      parsed.bits = 0;
      if (parsed.algorithm == AlgorithmRsa || parsed.algorithm == AlgorithmRsaEncryptOnly ||
          parsed.algorithm == AlgorithmRsaSignOnly || parsed.algorithm == AlgorithmElgamal ||
          parsed.algorithm == AlgorithmDsa)
      {
         if (material_at + 2 > length)
         {
            error = "The key's public-key packet ends before its first number.";
            return false;
         }
         parsed.bits = (int) ReadBigEndian16(body + material_at);
         if (parsed.bits <= 0 || parsed.bits > 65535)
         {
            error = "The key's first number has an impossible size.";
            return false;
         }
      }
      else if (parsed.algorithm == AlgorithmEd25519 || parsed.algorithm == AlgorithmX25519)
         parsed.bits = 256;
      else if (parsed.algorithm == AlgorithmEd448)
         parsed.bits = 456;
      else if (parsed.algorithm == AlgorithmX448)
         parsed.bits = 448;

      // The fingerprint: a hash over a prefix octet, the length of the packet
      // body, and the body. The octet and the hash are what the version picks.
      std::vector<unsigned char> preimage;
      if (version == 4)
      {
         if (length > 0xFFFF)
         {
            error = "The key's public-key packet is too long for a version 4 fingerprint.";
            return false;
         }
         preimage.push_back(0x99);
         preimage.push_back((unsigned char) ((length >> 8) & 0xFF));
         preimage.push_back((unsigned char) (length & 0xFF));
      }
      else
      {
         preimage.push_back(version == 5 ? (unsigned char) 0x9A : (unsigned char) 0x9B);
         preimage.push_back((unsigned char) ((length >> 24) & 0xFF));
         preimage.push_back((unsigned char) ((length >> 16) & 0xFF));
         preimage.push_back((unsigned char) ((length >> 8) & 0xFF));
         preimage.push_back((unsigned char) (length & 0xFF));
      }
      preimage.insert(preimage.end(), body, body + length);

      if (version == 4)
      {
         unsigned char digest[SHA_DIGEST_LENGTH] = {};
         SHA1(&preimage[0], preimage.size(), digest);
         parsed.fingerprint = HexOf_(digest, sizeof(digest));
         parsed.key_id = HexOf_(digest + sizeof(digest) - 8, 8);
      }
      else
      {
         unsigned char digest[SHA256_DIGEST_LENGTH] = {};
         SHA256(&preimage[0], preimage.size(), digest);
         parsed.fingerprint = HexOf_(digest, sizeof(digest));
         parsed.key_id = HexOf_(digest, 8);
      }

      return true;
   }

   // Where the public key material of a key packet ends, so that a packet can
   // be required to end there too. A version 5 or 6 packet says (RFC 9580
   // section 5.5.2: a four-octet count before the material). A version 4
   // packet does not, so its material is walked field by field as section
   // 5.5.5 lays it out: an MPI is a two-octet bit count and that many bits
   // rounded up to octets; a curve is one length octet and its OID; the ECDH
   // KDF parameters are one length octet and that many octets; the four
   // native curve algorithms are a fixed number of octets.
   //
   // Why this matters: a secret-key packet is a public-key packet with the
   // secret material after the public, and its tag is one byte. Relabelling
   // that byte (5 to 6, 7 to 14) turns a secret key into something that
   // opens like a public one, and the only thing that gives it away is the
   // bytes after the public material. A version 4 packet of an algorithm this
   // server cannot walk is the one shape that cannot be checked; it is
   // accepted as it always was, and the header of this file says so.
   bool
   OpenPgpKey::PublicMaterialEnd_(const unsigned char *body, size_t length, int version, int algorithm,
                                  size_t &material_at, size_t &material_end, AnsiString &error)
   {
      material_at = 6;
      material_end = 0;

      if (version == 5 || version == 6)
      {
         if (length < 10)
         {
            error = "The key's public-key packet ends before the length of its key material.";
            return false;
         }
         size_t material_length = ReadBigEndian32(body + 6);
         material_at = 10;
         if (material_length > length - material_at)
         {
            error = "The key's public-key packet says its key material is longer than the packet.";
            return false;
         }
         material_end = material_at + material_length;
         return true;
      }

      size_t at = material_at;
      bool fits = true;

      auto mpi = [&]()
      {
         if (at + 2 > length)
         {
            fits = false;
            return;
         }
         size_t bits = ReadBigEndian16(body + at);
         at += 2 + (bits + 7) / 8;
         if (at > length)
            fits = false;
      };

      auto counted = [&]()
      {
         if (at + 1 > length)
         {
            fits = false;
            return;
         }
         at += 1 + (size_t) body[at];
         if (at > length)
            fits = false;
      };

      auto fixed = [&](size_t octets)
      {
         at += octets;
         if (at > length)
            fits = false;
      };

      switch (algorithm)
      {
      case AlgorithmRsa:
      case AlgorithmRsaEncryptOnly:
      case AlgorithmRsaSignOnly:
         mpi(); mpi();                       // n, e
         break;
      case AlgorithmElgamal:
         mpi(); mpi(); mpi();                // p, g, y
         break;
      case AlgorithmDsa:
         mpi(); mpi(); mpi(); mpi();         // p, q, g, y
         break;
      case AlgorithmEcdsa:
      case AlgorithmEddsaLegacy:
         counted(); mpi();                   // curve OID, point
         break;
      case AlgorithmEcdh:
         counted(); mpi(); counted();        // curve OID, point, KDF parameters
         break;
      case AlgorithmX25519:
      case AlgorithmEd25519:
         fixed(32);
         break;
      case AlgorithmX448:
         fixed(56);
         break;
      case AlgorithmEd448:
         fixed(57);
         break;
      default:
         // An algorithm this server cannot walk: the packet is taken to end
         // where its material does, because there is no way to say otherwise.
         material_end = length;
         return true;
      }

      if (!fits)
      {
         error = "The key's public-key packet ends before the end of its key material.";
         return false;
      }

      material_end = at;
      return true;
   }

   // A subkey packet (tag 14) is read for the same reason the primary key is:
   // it must be a public key and nothing more. Its version, algorithm and
   // fingerprint are not kept - the primary key is the key - but its body has
   // to end where its public material does.
   bool
   OpenPgpKey::ReadSubkeyPacket_(const unsigned char *data, const Packet &packet, AnsiString &error)
   {
      const unsigned char *body = data + packet.body_start;
      size_t length = packet.body_length;

      if (length < 6)
      {
         error = "A subkey packet in this key is too short to be a public key.";
         return false;
      }

      int version = body[0];
      if (version != 4 && version != 5 && version != 6)
      {
         error.Format("A subkey in this key says it is version %d, and this server reads versions 4, 5 and 6.", version);
         return false;
      }

      size_t material_at = 6;
      size_t material_end = 0;
      AnsiString inner;
      if (!PublicMaterialEnd_(body, length, version, body[5], material_at, material_end, inner))
      {
         error = "A subkey packet in this key does not add up: " + inner;
         return false;
      }

      if (material_end != length)
      {
         error = "A subkey packet in this key carries bytes after its public key material. A public-subkey packet ends where its key material does; a secret-subkey packet whose tag has been changed to a public one does not, and this server never holds one.";
         return false;
      }

      return true;
   }

   // What one signature packet says about the key it is attached to: whether it
   // claims to have been made by this key, whether it marks the user id it
   // certifies as the primary one, and how long it says the key lives.
   void
   OpenPgpKey::ReadSignature_(const unsigned char *data, const Packet &packet, const Parsed &key,
                              bool &is_self_certification, bool &is_primary_flag, __int64 &expiry_seconds)
   {
      is_self_certification = false;
      is_primary_flag = false;
      expiry_seconds = -1;

      const unsigned char *body = data + packet.body_start;
      size_t length = packet.body_length;

      if (length < 6)
         return;

      int version = body[0];
      if (version != 4 && version != 6)
         return;

      int type = body[1];

      size_t at = 0;
      size_t hashed_length = 0;
      if (version == 4)
      {
         hashed_length = ReadBigEndian16(body + 4);
         at = 6;
      }
      else
      {
         if (length < 8)
            return;
         hashed_length = ReadBigEndian32(body + 4);
         at = 8;
      }

      if (hashed_length > length - at)
         return;

      size_t hashed_start = at;
      size_t hashed_end = at + hashed_length;
      at = hashed_end;

      size_t unhashed_length = 0;
      if (version == 4)
      {
         if (at + 2 > length)
            return;
         unhashed_length = ReadBigEndian16(body + at);
         at += 2;
      }
      else
      {
         if (at + 4 > length)
            return;
         unhashed_length = ReadBigEndian32(body + at);
         at += 4;
      }

      if (unhashed_length > length - at)
         return;

      size_t unhashed_start = at;
      size_t unhashed_end = at + unhashed_length;

      bool issued_by_this_key = false;

      // The hashed area is the part the signature covers, so an expiry and a
      // primary-user-id flag are read from it and from nowhere else. The issuer
      // is read from both, because that is where every implementation puts it.
      for (int area = 0; area < 2; area++)
      {
         size_t walk = area == 0 ? hashed_start : unhashed_start;
         size_t end = area == 0 ? hashed_end : unhashed_end;

         int subpacket_type = 0;
         const unsigned char *value = nullptr;
         size_t value_length = 0;
         while (NextSubpacket(body, end, walk, subpacket_type, value, value_length))
         {
            if (subpacket_type == 16 && value_length == 8)
            {
               if (HexOf_(value, 8) == key.key_id)
                  issued_by_this_key = true;
            }
            else if (subpacket_type == 33 && value_length >= 21)
            {
               // One octet of key version, then the fingerprint itself.
               if (HexOf_(value + 1, value_length - 1) == key.fingerprint)
                  issued_by_this_key = true;
            }
            else if (area == 0 && subpacket_type == 9 && value_length == 4)
               expiry_seconds = (__int64) ReadBigEndian32(value);
            else if (area == 0 && subpacket_type == 25 && value_length == 1)
               is_primary_flag = value[0] != 0;
         }
      }

      // 0x10 to 0x13 certify a user id; 0x1F signs the key itself. Either is a
      // self-signature when this key issued it.
      bool certifies = (type >= 0x10 && type <= 0x13) || type == 0x1F;
      is_self_certification = certifies && issued_by_this_key;
   }

   unsigned int
   OpenPgpKey::ArmourChecksum(const unsigned char *data, size_t length)
   {
      return Crc24(data, length);
   }

   OpenPgpKey::Parsed
   OpenPgpKey::Parse(const unsigned char *data, size_t length, size_t maximum)
   {
      Parsed parsed;

      if (maximum == 0)
         maximum = DefaultMaximumBytes;

      if (length == 0)
      {
         parsed.error = "There is no key here: the upload is empty.";
         return parsed;
      }

      if (length > maximum)
      {
         parsed.error.Format("This key is %d bytes and this server publishes keys of at most %d. A key that large is a keyring or a key with a very large number of third-party signatures on it; export it with only its own certifications.",
                             (int) length, (int) maximum);
         return parsed;
      }

      Packet first;
      AnsiString error;
      if (!ReadPacket_(data, length, 0, first, error))
      {
         parsed.error = error;
         return parsed;
      }

      if (first.tag == TagSecretKey || first.tag == TagSecretSubkey)
      {
         parsed.error = "This is a SECRET key, and this server never holds one. Export the public half of it - gpg --export, not gpg --export-secret-keys - and upload that.";
         return parsed;
      }

      if (first.tag == TagPublicSubkey)
      {
         parsed.error = "This is a subkey on its own. Export the whole key, primary key first, and upload that.";
         return parsed;
      }

      if (first.tag != TagPublicKey)
      {
         parsed.error.Format("These bytes open with an OpenPGP packet of type %d, and a public key opens with a public-key packet.", first.tag);
         return parsed;
      }

      if (!ReadKeyPacket_(data, first, parsed, error))
      {
         parsed.error = error;
         return parsed;
      }

      // The walk. Everything after the primary key is a certification, a user
      // id, a user attribute or a subkey with its signatures - and the last
      // packet has to end exactly where the bytes do.
      bool has_self_signature = false;
      __int64 expiry_from_primary = -1;
      __int64 expiry_from_any = -1;
      AnsiString pending_user_id;
      bool pending_user_id_open = false;

      size_t at = first.next;
      while (at < length)
      {
         Packet packet;
         if (!ReadPacket_(data, length, at, packet, error))
         {
            parsed.error = error;
            return parsed;
         }

         switch (packet.tag)
         {
         case TagSecretKey:
         case TagSecretSubkey:
            parsed.error = "This key carries a SECRET key packet, and this server never holds one. Export the public half of it - gpg --export, not gpg --export-secret-keys - and upload that.";
            return parsed;

         case TagPublicKey:
            parsed.error = "These bytes carry more than one key. Upload one key at a time; a keyring is not a Web Key Directory answer.";
            return parsed;

         case TagTrust:
            parsed.error = "This key carries a trust packet, which belongs to a local keyring and is never part of an exported key. Export it again with gpg --export.";
            return parsed;

         case TagUserId:
            {
               AnsiString text((const char *) (data + packet.body_start), (int) packet.body_length);
               parsed.user_ids.push_back(text);
               parsed.user_id_count++;
               pending_user_id = text;
               pending_user_id_open = true;
               break;
            }

         case TagUserAttribute:
            pending_user_id_open = false;
            break;

         case TagPublicSubkey:
            if (!ReadSubkeyPacket_(data, packet, error))
            {
               parsed.error = error;
               return parsed;
            }
            parsed.subkey_count++;
            pending_user_id_open = false;
            break;

         case TagSignature:
            {
               parsed.signature_count++;

               bool self_signature = false;
               bool primary_flag = false;
               __int64 expiry = -1;
               ReadSignature_(data, packet, parsed, self_signature, primary_flag, expiry);

               if (self_signature)
               {
                  has_self_signature = true;

                  if (expiry >= 0 && expiry_from_any < 0)
                     expiry_from_any = expiry;

                  // The primary user id is the one a self-signature marks as
                  // primary; failing that, the first one this key certified.
                  if (pending_user_id_open && (primary_flag || parsed.primary_user_id.IsEmpty()))
                  {
                     parsed.primary_user_id = pending_user_id;
                     expiry_from_primary = expiry;
                  }
               }
               break;
            }

         case TagPadding:
            break;

         default:
            parsed.error.Format("This key carries an OpenPGP packet of type %d, which is not part of a public key.", packet.tag);
            return parsed;
         }

         at = packet.next;
      }

      if (at != length)
      {
         parsed.error = "The packets in this key do not add up: there are bytes after the last one.";
         return parsed;
      }

      if (parsed.user_id_count == 0)
      {
         parsed.error = "This key carries no user id, so there is no address it could be published for.";
         return parsed;
      }

      if (!has_self_signature)
      {
         parsed.error = "This key carries no self-signature: nothing in it says that the key itself certified any of its user ids. An export of a key that has never been self-certified is not a usable key.";
         return parsed;
      }

      if (parsed.primary_user_id.IsEmpty())
         parsed.primary_user_id = parsed.user_ids[0];

      __int64 expiry_seconds = expiry_from_primary >= 0 ? expiry_from_primary : expiry_from_any;
      parsed.expires = expiry_seconds > 0 ? parsed.created + expiry_seconds : 0;

      parsed.bytes.assign(data, data + length);
      parsed.size = length;
      parsed.ok = true;
      return parsed;
   }

   bool
   OpenPgpKey::Dearmour(const AnsiString &text, std::vector<unsigned char> &out, AnsiString &error)
   {
      out.clear();

      std::string source(text.c_str(), (size_t) text.GetLength());

      if (source.find("-----BEGIN PGP PRIVATE KEY BLOCK-----") != std::string::npos ||
          source.find("-----BEGIN PGP SECRET KEY BLOCK-----") != std::string::npos)
      {
         error = "This is a SECRET key, and this server never holds one. Export the public half of it - gpg --armor --export, not gpg --armor --export-secret-keys - and upload that.";
         return false;
      }

      return ReadArmour_(source, "PUBLIC KEY BLOCK", out, error);
   }

   bool
   OpenPgpKey::DearmourMessage(const AnsiString &text, std::vector<unsigned char> &out, AnsiString &error)
   {
      const std::string source(text.c_str(), (size_t) text.GetLength());
      return ReadArmour_(source, "MESSAGE", out, error);
   }

   // The radix-64 block of RFC 9580 section 6, for whichever label the caller
   // wants. Dearmour is the public key's door and refuses a secret key before
   // it reaches here; ReadSecretKey is the other caller, and the one thing it
   // asks of a secret key is that it be encrypted.
   bool
   OpenPgpKey::ReadArmour_(const std::string &source, const std::string &label, std::vector<unsigned char> &out, AnsiString &error)
   {
      out.clear();

      const std::string beginLine = "-----BEGIN PGP " + label + "-----";
      const std::string endLine = "-----END PGP " + label + "-----";

      size_t begin = source.find(beginLine);
      if (begin == std::string::npos)
      {
         error.Format("This armoured block is not a PGP %s.", label.c_str());
         return false;
      }

      size_t end = source.find(endLine, begin);
      if (end == std::string::npos)
      {
         error = "This armoured block has no end line.";
         return false;
      }

      // The body opens after the header line, then any Armor Header lines, then
      // one blank line (RFC 9580 section 6.2).
      size_t at = source.find('\n', begin);
      if (at == std::string::npos || at > end)
      {
         error = "This armoured block has no line after its header.";
         return false;
      }
      at++;

      while (at < end)
      {
         size_t lineEnd = source.find('\n', at);
         if (lineEnd == std::string::npos || lineEnd > end)
            lineEnd = end;

         std::string line = source.substr(at, lineEnd - at);
         while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t'))
            line.erase(line.size() - 1);

         at = lineEnd + 1;

         if (line.empty())
            break;
         if (line.find(':') == std::string::npos)
         {
            error = "This armoured block has no blank line between its headers and its data.";
            return false;
         }
      }

      std::string data;
      std::string checksum;
      while (at < end)
      {
         size_t lineEnd = source.find('\n', at);
         if (lineEnd == std::string::npos || lineEnd > end)
            lineEnd = end;

         std::string line = source.substr(at, lineEnd - at);
         while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' ' || line[line.size() - 1] == '\t'))
            line.erase(line.size() - 1);

         at = lineEnd + 1;

         if (line.empty())
            continue;
         if (line[0] == '=')
            checksum = line.substr(1);
         else
            data += line;
      }

      if (!DecodeRadix64(data, out))
      {
         error = "The data inside this armoured block is not radix-64.";
         return false;
      }

      if (out.empty())
      {
         error = "This armoured block carries no data.";
         return false;
      }

      // The checksum is optional in RFC 9580, and it is checked when it is
      // there: a truncated paste is the commonest way a key arrives broken, and
      // this is what catches it before the packet walk does.
      if (!checksum.empty())
      {
         std::vector<unsigned char> expected;
         if (!DecodeRadix64(checksum, expected) || expected.size() != 3)
         {
            error = "The checksum line of this armoured block is not readable.";
            return false;
         }

         unsigned int computed = Crc24(&out[0], out.size());
         unsigned int given = ((unsigned int) expected[0] << 16) | ((unsigned int) expected[1] << 8) | (unsigned int) expected[2];
         if (computed != given)
         {
            error = "The checksum of this armoured block does not match its data, so the block was cut short or altered on the way here.";
            out.clear();
            return false;
         }
      }

      return true;
   }

   // A SECRET key, read for one question and one question only: is every
   // secret packet in it encrypted under a passphrase?
   //
   // This server holds the account's secret OpenPGP key for the webmail, and
   // holds it in the OpenPGP format so that the same bytes open in GnuPG. That
   // is only safe while the key inside is passphrase-encrypted, and the
   // encryption happens in the browser where this server cannot see it - so
   // this is the one place that can refuse a key that arrived without it. A
   // secret-key packet whose string-to-key usage octet is 0 is a private key in
   // the clear, and it is refused by name rather than stored.
   //
   // Nothing here decrypts anything, and no passphrase is involved: the usage
   // octet sits immediately after the public key material, which
   // PublicMaterialEnd_ already knows how to find.
   OpenPgpKey::SecretKeyFacts
   OpenPgpKey::ReadSecretKey(const AnsiString &text, size_t maximum)
   {
      SecretKeyFacts facts;

      if (maximum == 0)
         maximum = DefaultMaximumBytes;

      AnsiString trimmed = text;
      trimmed.Trim();
      std::string source(trimmed.c_str(), (size_t) trimmed.GetLength());

      std::vector<unsigned char> bytes;
      AnsiString error;
      if (!ReadArmour_(source, "PRIVATE KEY BLOCK", bytes, error) &&
          !ReadArmour_(source, "SECRET KEY BLOCK", bytes, error))
      {
         facts.error = "This is not an armoured PGP PRIVATE KEY BLOCK.";
         return facts;
      }

      if (bytes.size() > maximum)
      {
         facts.error.Format("This secret key is %d bytes, and this server holds one of at most %d.",
                            (int) bytes.size(), (int) maximum);
         return facts;
      }

      const unsigned char *data = &bytes[0];
      const size_t length = bytes.size();

      size_t at = 0;
      bool sawPrimary = false;
      int userIds = 0;
      while (at < length)
      {
         Packet packet;
         if (!ReadPacket_(data, length, at, packet, facts.error))
            return facts;

         if (packet.tag == TagSecretKey || packet.tag == TagSecretSubkey)
         {
            const unsigned char *body = data + packet.body_start;
            const size_t bodyLength = packet.body_length;
            if (bodyLength < 7)
            {
               facts.error = "A secret-key packet in this key is too short to be one.";
               return facts;
            }

            const int version = body[0];
            if (version != 4)
            {
               facts.error.Format("This secret key is version %d, and the webmail holds version 4 keys - the ones GnuPG 2.4 makes by default.", version);
               return facts;
            }

            size_t material_at = 6;
            size_t material_end = 0;
            if (!PublicMaterialEnd_(body, bodyLength, version, body[5], material_at, material_end, facts.error))
               return facts;

            if (material_end >= bodyLength)
            {
               facts.error = "A secret-key packet in this key carries no secret material.";
               return facts;
            }

            facts.secret_packets++;
            const int usage = body[material_end];
            if (usage == 0)
               facts.unprotected_packets++;

            if (packet.tag == TagSecretKey)
            {
               if (sawPrimary)
               {
                  facts.error = "This block holds more than one key. One upload is one key.";
                  return facts;
               }
               sawPrimary = true;
               facts.version = version;
               facts.algorithm = body[5];

               std::vector<unsigned char> preimage;
               preimage.push_back(0x99);
               preimage.push_back((unsigned char) ((material_end >> 8) & 0xFF));
               preimage.push_back((unsigned char) (material_end & 0xFF));
               preimage.insert(preimage.end(), body, body + material_end);

               unsigned char digest[SHA_DIGEST_LENGTH] = {};
               SHA1(&preimage[0], preimage.size(), digest);
               facts.fingerprint = HexOf_(digest, sizeof(digest));
               facts.key_id = HexOf_(digest + sizeof(digest) - 8, 8);
            }
         }
         else if (packet.tag == TagUserId)
         {
            userIds++;
            if (facts.primary_user_id.IsEmpty())
               facts.primary_user_id = AnsiString((const char *) (data + packet.body_start), (int) packet.body_length);
         }
         else if (packet.tag == TagPublicKey)
         {
            facts.error = "This block holds a PUBLIC key packet; a secret key is what belongs here.";
            return facts;
         }

         at = packet.next;
      }

      if (!sawPrimary)
      {
         facts.error = "This block holds no secret key.";
         return facts;
      }
      if (!userIds)
      {
         facts.error = "This secret key carries no user id.";
         return facts;
      }
      if (facts.unprotected_packets > 0)
      {
         facts.error = "This secret key is NOT encrypted: its private numbers are in the clear. A key stored here is encrypted in the browser under a passphrase this server never sees, and one that arrived without that encryption is refused rather than kept.";
         return facts;
      }

      facts.ok = true;
      return facts;
   }

   OpenPgpKey::Parsed
   OpenPgpKey::ParseText(const AnsiString &text, size_t maximum)
   {
      Parsed parsed;

      AnsiString trimmed = text;
      trimmed.Trim();

      if (trimmed.IsEmpty())
      {
         parsed.error = "There is no key here: the upload is empty.";
         return parsed;
      }

      std::string source(trimmed.c_str(), (size_t) trimmed.GetLength());

      if (source.find("-----BEGIN PGP ") != std::string::npos)
      {
         std::vector<unsigned char> bytes;
         AnsiString error;
         if (!Dearmour(trimmed, bytes, error))
         {
            parsed.error = error;
            return parsed;
         }
         return Parse(bytes.empty() ? nullptr : &bytes[0], bytes.size(), maximum);
      }

      std::vector<unsigned char> bytes;
      if (!DecodeRadix64(source, bytes) || bytes.empty())
      {
         parsed.error = "This is neither an armoured PGP PUBLIC KEY BLOCK nor base64 of a key's bytes.";
         return parsed;
      }

      return Parse(&bytes[0], bytes.size(), maximum);
   }

   AnsiString
   OpenPgpKey::ZBase32(const unsigned char *data, size_t length)
   {
      static const char alphabet[] = "ybndrfg8ejkmcpqxot1uwisza345h769";

      AnsiString out;
      unsigned int accumulator = 0;
      int bits = 0;

      for (size_t i = 0; i < length; i++)
      {
         accumulator = (accumulator << 8) | data[i];
         bits += 8;
         while (bits >= 5)
         {
            bits -= 5;
            out += alphabet[(accumulator >> bits) & 0x1F];
         }
      }

      // The last few bits are padded with zeroes on the right, which is what
      // makes 20 octets of SHA-1 exactly 32 characters with nothing left over.
      if (bits > 0)
         out += alphabet[(accumulator << (5 - bits)) & 0x1F];

      return out;
   }

   AnsiString
   OpenPgpKey::HashedLocalPart(const AnsiString &localPart)
   {
      // Only A to Z are folded, by hand: the draft's section 3.1 lower-cases
      // the ASCII letters and leaves every other octet as it is, and a locale
      // aware lower-casing would make the hash depend on the process's locale.
      std::string lowered(localPart.c_str(), (size_t) localPart.GetLength());
      for (size_t i = 0; i < lowered.size(); i++)
      {
         if (lowered[i] >= 'A' && lowered[i] <= 'Z')
            lowered[i] = (char) (lowered[i] - 'A' + 'a');
      }

      unsigned char digest[SHA_DIGEST_LENGTH] = {};
      SHA1((const unsigned char *) lowered.data(), lowered.size(), digest);

      return ZBase32(digest, sizeof(digest));
   }

   AnsiString
   OpenPgpKey::AddressOfUserId(const AnsiString &userId)
   {
      AnsiString address;

      int open = userId.ReverseFind('<');
      if (open >= 0)
      {
         int close = userId.Find(">", open);
         if (close > open)
            address = userId.Mid(open + 1, close - open - 1);
      }
      else
         address = userId;

      address.Trim();
      address.MakeLower();

      if (address.Find("@") <= 0 || address.Find(" ") >= 0)
         return "";

      return address;
   }

   bool
   OpenPgpKey::CarriesAddress(const Parsed &key, const AnsiString &address)
   {
      AnsiString wanted = address;
      wanted.Trim();
      wanted.MakeLower();

      if (wanted.IsEmpty())
         return false;

      for (size_t i = 0; i < key.user_ids.size(); i++)
      {
         if (AddressOfUserId(key.user_ids[i]) == wanted)
            return true;
      }

      return false;
   }

   // ---------------------------------------------------------------------------
   // The tests. Every input here is a value, so this runs in process with no
   // bench: ClassTester calls it, and the suite reaches it through
   // MainOperations.TestInternals.
   //
   // WHERE THE VECTORS COME FROM. The two version 4 keys were made by GnuPG
   // 2.4.7 and exported with gpg --export; the fingerprints and key ids asserted
   // below are the ones gpg itself printed, so they are a cross-implementation
   // check and not this file agreeing with itself. The version 6 key is
   // synthetic - no shipping implementation makes one yet - and its fingerprint
   // was computed with Python's hashlib over the preimage RFC 9580 section 5.5.4
   // defines, again outside this code. Neither address exists.
   // ---------------------------------------------------------------------------

   namespace
   {
      void Require(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      // "Joe Doe <joe.doe@example.org>", Ed25519 with a Curve25519 subkey, no
      // expiry. gpg says the fingerprint is C29B6CCE361E73BA13724CB8BBEE452B9B0A0BB7.
      const char *JoeKeyBase64 =
         "mDMEaqoCExYJKwYBBAHaRw8BAQdA1BnI224Vt/ATv5Bz3LFdMeQvJKTMb+poSTbQQ/LMCbq0HUpv"
         "ZSBEb2UgPGpvZS5kb2VAZXhhbXBsZS5vcmc+iJMEExYKADsWIQTCm2zONh5zuhNyTLi77kUrmwoL"
         "twUCaqoCEwIbAwULCQgHAgIiAgYVCgkICwIEFgIDAQIeBwIXgAAKCRC77kUrmwoLt58WAQDdPjL0"
         "sHXuW7D6eqbmCmbURqQ9Ov8scnFyKsEPkA85lAD/VT/yJn8zo+3dCGtmBudvsQbexnGQkjVW3Yzo"
         "jkj7RAi4OARqqgITEgorBgEEAZdVAQUBAQdAHKnYcC+ab6m2F5NyPeVPCwzQFzNIqPMaIkoeuyvE"
         "wzYDAQgHiHgEGBYKACAWIQTCm2zONh5zuhNyTLi77kUrmwoLtwUCaqoCEwIbDAAKCRC77kUrmwoL"
         "t4etAQDidGBnPZ3BKXDgd3iwtPNfDxmdaae4hRPJ6rBBXhGAdgD8DDeT9bI9oZqHeCt5mGu1DzJh"
         "zDzrRPuZ7gABvV9puQc=";

      // "Ann Example <ann@example.org>" and a second user id, RSA 2048, expiring
      // a year after it was made. gpg says 800B26CCC36E0E3B4A61A26B19FE7A2FE905DF20.
      const char *AnnKeyBase64 =
         "mQENBGqqAiEBCADFsurRCxIYwqBxuwUf9vktSLk3PkTNlDCklnFzS7R18aq/nnKVfK6iQK1YHFxr"
         "Tam53W9V7mH8H68uysFupWca7Btar+fX4LhmTeo6A44yiZXQlw6u6hZisA/GaLWsuGKvFQVBufbd"
         "AsIfXByY5ZVdDnF35DaEJPKZUESWXPHedISljkYB2gl7MtvNF5uMnKnDlzQNS+VRiEXhf9pTltLm"
         "wulM7c7pNzHpTw+kC1bvG5uhJwEXFes/f02rzWorXvrz8+ZP3/+kOgQp6B6jeORAsioG8HERC/us"
         "oR93PiUiDPuAMvSLVDsaY6hJdD7S1VXnz3wPj7ZL+ABHUaU07mjHABEBAAG0HUFubiBFeGFtcGxl"
         "IDxhbm5AZXhhbXBsZS5vcmc+iQFXBBMBCABBFiEEgAsmzMNuDjtKYaJrGf56L+kF3yAFAmqqAiEC"
         "GwMFCQHhM4AFCwkIBwICIgIGFQoJCAsCBBYCAwECHgcCF4AACgkQGf56L+kF3yAcLwgAoLJBC96F"
         "/vOtmOu5DCelFeiRGWxYiq4Rm0W9KN/cHYNvTNamD5BSb/aiXXZqzlbGvMA9zqebYvxMStX/OLZt"
         "8TBLxjV5bV3QAykDtATJW1T7tmqjgy355E9yeRd06t+kBmSmmY6HiSykqd+9Z9YfGyiygOlpKA+N"
         "Kk7qxFBrcL4w6UOWBdM2w7X65eR9BcDydfMJHrwNS/Gtv/3ZrVclCXrk5E1+BL0cKL+TSbUXjns+"
         "LhSc69ggi3D60H/a8R/pWcCPTFmplpeCFO+ygwS8bOoSv9LdZ09CjSuPwUmrzHHKsMVMuA11wNPi"
         "moIlj+S8cqLYiQfsP1srRTfDgWeIfLQhQW5uIEUgRXhhbXBsZSA8YW5uLmVAZXhhbXBsZS5vcmc+"
         "iQFXBBMBCABBFiEEgAsmzMNuDjtKYaJrGf56L+kF3yAFAmqqAiUCGwMFCQHhM4AFCwkIBwICIgIG"
         "FQoJCAsCBBYCAwECHgcCF4AACgkQGf56L+kF3yDkWgf/T1/tjVq1T6G+LZMwt0LIF5pbqBglDqfm"
         "c6xqr4huHQA9tAhqupAT7o/mfRxqRD5CdfzRTfY087Gv9u51KAkRvEbMjnq6oWxQ+YDwtotFSZeY"
         "TTFChNTkrM2WLRjhipoJW7/+ulcYVE4VUDmDkIChaApsT90m28GGqcvDiQhxsyxPV6xy3VqJOFg7"
         "kOS/A3/GANaOgk7D9aPy44JcnDTlCNZksU4JkktHY2GCyB+5Pjk3ZI9Fq+omDecdokfn67868rDv"
         "84C6PqbVF7xiuKYsU4z63S/Kzj6TCDVe20Hi8T/ylRQE8PV9zHacExOkEIj/vwpo2WPcWps/meuw"
         "owKaAQ==";

      // A version 6 key: Ed25519, one user id, one self-signature carrying a
      // thirty-day expiry. Made by build's scratch generator; its fingerprint
      // came out of hashlib, not out of this file.
      const char *VeraKeyBase64 =
         "xioGaqoCExsAAAAgAAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh/NH1ZlcmEgU2l4IDx2"
         "ZXJhLnNpeEBleGFtcGxlLm9yZz7CmAYTGwoAAAApIiEGoiBpX2cjCtcnnk4v0UU9J5uFUjNmVYxr"
         "1eyhCfkduUoFCQAnjQAAAAAAEjQgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
         "AAAA";

      std::vector<unsigned char> Bytes(const char *base64Text)
      {
         std::vector<unsigned char> bytes;
         if (!DecodeRadix64(std::string(base64Text), bytes))
            throw std::logic_error("A test vector in OpenPgpKey.cpp is not base64.");
         return bytes;
      }

      AnsiString Armour(const char *base64Text, const char *checksumLine)
      {
         AnsiString armour = "-----BEGIN PGP PUBLIC KEY BLOCK-----\r\n";
         armour += "Comment: a test\r\n";
         armour += "\r\n";
         armour += base64Text;
         armour += "\r\n";
         if (checksumLine != nullptr)
         {
            armour += checksumLine;
            armour += "\r\n";
         }
         armour += "-----END PGP PUBLIC KEY BLOCK-----\r\n";
         return armour;
      }
   }

   void
   OpenPgpKeyTester::Test()
   {
      // ---- the hashed local part, against the draft's own example
      {
         Require(OpenPgpKey::HashedLocalPart("Joe.Doe") == "iy9q119eutrkn8s1mk4r39qejnbu3n5q",
                 "The hashed local part of Joe.Doe is not the value in draft-koch-openpgp-webkey-service.");
         Require(OpenPgpKey::HashedLocalPart("joe.doe") == "iy9q119eutrkn8s1mk4r39qejnbu3n5q",
                 "The hashed local part is not case-folded before hashing.");
         Require(OpenPgpKey::HashedLocalPart("joe.doe").GetLength() == 32,
                 "A hashed local part is not 32 characters of z-base-32.");
         Require(OpenPgpKey::HashedLocalPart("joe.doe") != OpenPgpKey::HashedLocalPart("joe.doa"),
                 "Two different local parts hashed to the same thing.");

         // A plus sign is an octet like any other, and only A to Z are folded:
         // the two spellings of one German name differ in the case of a non-ASCII
         // letter and must hash differently, because the draft folds ASCII and
         // nothing else. The values are hashlib's.
         Require(OpenPgpKey::HashedLocalPart("Ann+Lists") == "knpybb6xjyrgg6wsz7bhbx8e9kzr8y8s",
                 "The hashed local part of Ann+Lists is not hashlib's.");
         Require(OpenPgpKey::HashedLocalPart("J\xC3\xBCrgen") == "xotup5kjnwdgxj1qa4a6s1j1hx3q5196",
                 "A UTF-8 local part was not hashed octet for octet.");
         Require(OpenPgpKey::HashedLocalPart("J\xC3\x9Crgen") == "bbci4p578ntucorruusqkfa8todycfkg",
                 "A non-ASCII capital was folded, and the draft folds ASCII letters only.");

         // The alphabet itself, five bits at a time: 0x00 is 'y' and 0xFF is the
         // last-but-one letter of the alphabet followed by the padding of the
         // three bits that are left.
         const unsigned char zero[1] = { 0x00 };
         Require(OpenPgpKey::ZBase32(zero, 1) == "yy", "z-base-32 of one zero octet is not yy.");
      }

      // ---- a real version 4 key: Ed25519 with a subkey
      {
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);
         OpenPgpKey::Parsed key = OpenPgpKey::Parse(&bytes[0], bytes.size(), 0);

         Require(key.ok, "A key GnuPG exported was refused.");
         Require(key.version == 4, "A version 4 key was not read as version 4.");
         Require(key.fingerprint == "c29b6cce361e73ba13724cb8bbee452b9b0a0bb7",
                 "The version 4 fingerprint is not the one GnuPG prints for this key.");
         Require(key.key_id == "bbee452b9b0a0bb7", "The version 4 key id is not the one GnuPG prints.");
         Require(key.algorithm == OpenPgpKey::AlgorithmEddsaLegacy, "The key's algorithm was read wrongly.");
         Require(key.algorithm_name == "EdDSA", "The key's algorithm has no name.");
         Require(key.created == 1789526547, "The key's creation time was read wrongly.");
         Require(key.expires == 0, "A key with no expiry was given one.");
         Require(key.user_id_count == 1, "The key's one user id was not counted.");
         Require(key.primary_user_id == "Joe Doe <joe.doe@example.org>", "The key's primary user id is wrong.");
         Require(key.subkey_count == 1, "The key's encryption subkey was not counted.");
         Require(key.signature_count == 2, "The key's two signatures were not counted.");
         Require(key.size == bytes.size() && key.bytes.size() == bytes.size(),
                 "The parsed key is not the bytes that were given.");
         Require(OpenPgpKey::CarriesAddress(key, "JOE.DOE@example.org"),
                 "The key's own address was not found in its user id.");
         Require(!OpenPgpKey::CarriesAddress(key, "someone.else@example.org"),
                 "An address the key does not carry was found in it.");
      }

      // ---- a real version 4 key: RSA, two user ids, an expiry
      {
         OpenPgpKey::Parsed key = OpenPgpKey::ParseText(AnnKeyBase64, 0);

         Require(key.ok, "An RSA key GnuPG exported was refused.");
         Require(key.fingerprint == "800b26ccc36e0e3b4a61a26b19fe7a2fe905df20",
                 "The RSA key's fingerprint is not the one GnuPG prints.");
         Require(key.algorithm == OpenPgpKey::AlgorithmRsa && key.bits == 2048,
                 "The RSA key's modulus size was not read.");
         Require(key.created == 1789526561, "The RSA key's creation time was read wrongly.");
         Require(key.expires == 1789526561 + 31536000,
                 "The expiry in the self-signature was not added to the key's creation time.");
         Require(key.user_id_count == 2, "The key's two user ids were not counted.");
         Require(key.primary_user_id == "Ann Example <ann@example.org>",
                 "With no primary-user-id flag, the first self-certified user id is the primary one.");
         Require(OpenPgpKey::CarriesAddress(key, "ann.e@example.org"),
                 "The second user id's address was not found.");
      }

      // ---- a version 6 key is read, and is never refused for being one
      {
         OpenPgpKey::Parsed key = OpenPgpKey::ParseText(VeraKeyBase64, 0);

         Require(key.ok, "A version 6 key was refused.");
         Require(key.version == 6, "A version 6 key was not read as version 6.");
         Require(key.fingerprint == "a220695f67230ad7279e4e2fd1453d279b85523366558c6bd5eca109f91db94a",
                 "A version 6 fingerprint is a SHA-256 over 0x9B, the four-octet length and the packet body.");
         Require(key.fingerprint.GetLength() == 64, "A version 6 fingerprint is 64 hex characters.");
         Require(key.key_id == "a220695f67230ad7", "A version 6 key id is the first eight octets of its fingerprint.");
         Require(key.algorithm == OpenPgpKey::AlgorithmEd25519 && key.bits == 256,
                 "The version 6 key's algorithm was read wrongly.");
         Require(key.expires == 1789526547 + 30 * 86400,
                 "The expiry in a version 6 self-signature, whose subpacket areas are four octets long, was not read.");
         Require(key.primary_user_id == "Vera Six <vera.six@example.org>", "The version 6 key's user id was not read.");
      }

      // ---- a secret key is refused, by its packet and by its armour
      {
         // A secret-key packet (tag 5) with a body this parser never reaches:
         // the refusal is on the tag, before a byte of key material is read.
         const unsigned char secret[] = { 0xC5, 0x04, 0x04, 0x00, 0x00, 0x00 };
         OpenPgpKey::Parsed key = OpenPgpKey::Parse(secret, sizeof(secret), 0);
         Require(!key.ok, "A secret-key packet was accepted.");
         Require(key.error.Find("SECRET") >= 0, "The refusal of a secret key does not say that it is one.");

         AnsiString armoured = "-----BEGIN PGP PRIVATE KEY BLOCK-----\r\n\r\nAAAA\r\n-----END PGP PRIVATE KEY BLOCK-----\r\n";
         OpenPgpKey::Parsed fromArmour = OpenPgpKey::ParseText(armoured, 0);
         Require(!fromArmour.ok, "An armoured PRIVATE KEY BLOCK was accepted.");
         Require(fromArmour.error.Find("SECRET") >= 0,
                 "The refusal of an armoured private key does not say that it is one.");

         // A secret subkey hidden inside an otherwise good key.
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);
         bytes.push_back(0xC7);
         bytes.push_back(0x02);
         bytes.push_back(0x04);
         bytes.push_back(0x00);
         OpenPgpKey::Parsed mixed = OpenPgpKey::Parse(&bytes[0], bytes.size(), 0);
         Require(!mixed.ok && mixed.error.Find("SECRET") >= 0,
                 "A secret subkey appended to a public key was accepted.");
      }

      // ---- a secret key whose tag has been relabelled is caught by its bytes
      {
         // A secret-key packet is a public-key packet with the secret material
         // after the public, and its tag is one byte. Four bytes after the
         // public material stand in for that secret half, in each of the three
         // places it could be: the subkey, the primary key, and a version 6
         // primary key whose material count is right but whose packet is long.

         // The Joe key's subkey is the old-format packet at 233: 0xB8, a one
         // octet length of 56, then the body.
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);
         Require(bytes[233] == 0xB8 && bytes[234] == 56, "The Joe key's subkey is not where this test thinks it is.");
         std::vector<unsigned char> longSubkey = bytes;
         longSubkey[234] = 60;
         longSubkey.insert(longSubkey.begin() + 291, 4, (unsigned char) 0x00);
         OpenPgpKey::Parsed subkey = OpenPgpKey::Parse(&longSubkey[0], longSubkey.size(), 0);
         Require(!subkey.ok && subkey.error.Find("subkey") >= 0 && subkey.error.Find("after its public key material") >= 0,
                 "A subkey packet with bytes after its public material was accepted.");

         // The primary key is the old-format packet at 0: 0x98, a length of 51.
         Require(bytes[0] == 0x98 && bytes[1] == 51, "The Joe key's primary key is not where this test thinks it is.");
         std::vector<unsigned char> longPrimary = bytes;
         longPrimary[1] = 55;
         longPrimary.insert(longPrimary.begin() + 53, 4, (unsigned char) 0x00);
         OpenPgpKey::Parsed primary = OpenPgpKey::Parse(&longPrimary[0], longPrimary.size(), 0);
         Require(!primary.ok && primary.error.Find("after its public key material") >= 0,
                 "A version 4 key packet with bytes after its public material was accepted.");

         // The version 6 key opens with 0xC6, a one-octet length of 42, and
         // carries its material count: the count stays right, the packet grows.
         std::vector<unsigned char> vera = Bytes(VeraKeyBase64);
         Require(vera[0] == 0xC6 && vera[1] == 42, "The version 6 key is not where this test thinks it is.");
         vera[1] = 46;
         vera.insert(vera.begin() + 44, 4, (unsigned char) 0x00);
         OpenPgpKey::Parsed six = OpenPgpKey::Parse(&vera[0], vera.size(), 0);
         Require(!six.ok && six.error.Find("after its public key material") >= 0,
                 "A version 6 key packet longer than its material count was accepted.");

         // And the honest keys still add up: each material walk ends exactly
         // where its packet does, which is what the three refusals above rest on.
         Require(OpenPgpKey::Parse(&bytes[0], bytes.size(), 0).ok, "The Joe key was refused by the material walk.");
         Require(OpenPgpKey::ParseText(AnnKeyBase64, 0).ok, "The Ann key was refused by the material walk.");
         Require(OpenPgpKey::ParseText(VeraKeyBase64, 0).ok, "The Vera key was refused by the material walk.");
      }

      // ---- armour, with and without its checksum
      {
         OpenPgpKey::Parsed key = OpenPgpKey::ParseText(Armour(JoeKeyBase64, nullptr), 0);
         Require(key.ok && key.fingerprint == "c29b6cce361e73ba13724cb8bbee452b9b0a0bb7",
                 "An armoured public key block was not read.");

         OpenPgpKey::Parsed wrong = OpenPgpKey::ParseText(Armour(JoeKeyBase64, "=AAAA"), 0);
         Require(!wrong.ok && wrong.error.Find("checksum") >= 0,
                 "An armoured block whose CRC-24 does not match its data was accepted.");

         OpenPgpKey::Parsed notABlock = OpenPgpKey::ParseText("-----BEGIN PGP MESSAGE-----\r\n\r\nAAAA\r\n-----END PGP MESSAGE-----\r\n", 0);
         Require(!notABlock.ok, "An armoured MESSAGE was accepted as a key.");
      }

      // ---- the packets have to add up
      {
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);

         std::vector<unsigned char> truncated(bytes.begin(), bytes.end() - 20);
         OpenPgpKey::Parsed cut = OpenPgpKey::Parse(&truncated[0], truncated.size(), 0);
         Require(!cut.ok, "A truncated key was accepted.");

         std::vector<unsigned char> extra = bytes;
         extra.push_back(0x00);
         OpenPgpKey::Parsed trailing = OpenPgpKey::Parse(&extra[0], extra.size(), 0);
         Require(!trailing.ok, "A key with a byte after its last packet was accepted.");

         const unsigned char junk[] = { 0x41, 0x42, 0x43 };
         OpenPgpKey::Parsed notPackets = OpenPgpKey::Parse(junk, sizeof(junk), 0);
         Require(!notPackets.ok, "Three letters were accepted as a key.");
      }

      // ---- the shape of a transferable public key
      {
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);

         // The public-key packet on its own: no user id.
         std::vector<unsigned char> keyOnly(bytes.begin(), bytes.begin() + 53);
         OpenPgpKey::Parsed alone = OpenPgpKey::Parse(&keyOnly[0], keyOnly.size(), 0);
         Require(!alone.ok && alone.error.Find("user id") >= 0, "A key with no user id was accepted.");

         // The public-key packet and its user id, with the self-signature cut off.
         std::vector<unsigned char> unsigned_(bytes.begin(), bytes.begin() + 84);
         OpenPgpKey::Parsed noSignature = OpenPgpKey::Parse(&unsigned_[0], unsigned_.size(), 0);
         Require(!noSignature.ok && noSignature.error.Find("self-signature") >= 0,
                 "A key with no self-signature was accepted.");

         // A subkey on its own is not a key.
         std::vector<unsigned char> subkeyOnly(bytes.begin() + 233, bytes.begin() + 291);
         OpenPgpKey::Parsed subkey = OpenPgpKey::Parse(&subkeyOnly[0], subkeyOnly.size(), 0);
         Require(!subkey.ok && subkey.error.Find("subkey") >= 0, "A subkey on its own was accepted as a key.");

         // Two keys are a keyring, and a directory answers with one key.
         std::vector<unsigned char> two = bytes;
         two.insert(two.end(), bytes.begin(), bytes.end());
         OpenPgpKey::Parsed keyring = OpenPgpKey::Parse(&two[0], two.size(), 0);
         Require(!keyring.ok && keyring.error.Find("more than one key") >= 0, "A keyring was accepted as a key.");
      }

      // ---- the size cap
      {
         std::vector<unsigned char> bytes = Bytes(JoeKeyBase64);
         OpenPgpKey::Parsed capped = OpenPgpKey::Parse(&bytes[0], bytes.size(), 100);
         Require(!capped.ok, "A key larger than the cap was accepted.");
         Require(capped.error.Find("100") >= 0, "The refusal of an oversized key does not say what the cap is.");

         OpenPgpKey::Parsed empty = OpenPgpKey::Parse(nullptr, 0, 0);
         Require(!empty.ok, "An empty upload was accepted.");
      }

      // ---- the address a user id names
      {
         Require(OpenPgpKey::AddressOfUserId("Joe Doe <joe.doe@example.org>") == "joe.doe@example.org",
                 "The address inside the angle brackets was not read.");
         Require(OpenPgpKey::AddressOfUserId("JOE.DOE@EXAMPLE.ORG") == "joe.doe@example.org",
                 "A user id that is an address alone was not read.");
         Require(OpenPgpKey::AddressOfUserId("Joe Doe") == "",
                 "A user id with no address was read as one.");
         Require(OpenPgpKey::AddressOfUserId("Joe <a@b> and <joe@example.org>") == "joe@example.org",
                 "The LAST pair of angle brackets is the address, so a display name that contains one cannot be the answer.");
      }

      // ---- OUT-2: a key directory that never answered is held against the
      // organization, so a fresh subdomain of it is not another whole lookup
      // timeout on a REST worker. Nothing was held before.
      {
         OpenPgpRing::ForgetUnreachable();
         Require(!OpenPgpRing::DirectoryUnreachable(_T("a1.tarpit.example")), "A directory was held before anything failed.");
         OpenPgpRing::NoteDirectoryUnreachable(_T("a1.tarpit.example"));
         Require(OpenPgpRing::DirectoryUnreachable(_T("B2.Tarpit.Example")), "Another subdomain of the same organization is not held.");
         Require(OpenPgpRing::DirectoryUnreachable(_T("tarpit.example")), "The organization's own name is not held.");
         Require(!OpenPgpRing::DirectoryUnreachable(_T("tarpit2.example")), "Another organization is held.");
         Require(!OpenPgpRing::DirectoryUnreachable(_T("tarpit.example.net")), "A name that merely contains the organization is held.");
         OpenPgpRing::ForgetUnreachable();
         Require(!OpenPgpRing::DirectoryUnreachable(_T("b2.tarpit.example")), "Forgetting did not clear the hold.");
      }
   }
}
