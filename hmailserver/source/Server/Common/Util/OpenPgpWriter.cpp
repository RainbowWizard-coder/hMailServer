// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OpenPgpWriter.h"
#include "OpenPgpKey.h"
#include "Encoding/Base64.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <algorithm>
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
      const int TagPkesk = 1;
      const int TagSignature = 2;
      const int TagPublicKey = 6;
      const int TagLiteral = 11;
      const int TagUserId = 13;
      const int TagPublicSubkey = 14;
      const int TagSeipd = 18;

      const int AlgorithmRsa = 1;
      const int AlgorithmRsaEncryptOnly = 2;
      const int AlgorithmEcdh = 18;

      const int CipherAes256 = 9;

      // The cv25519 curve GnuPG names, as its OID appears in a key packet
      // (1.3.6.1.4.1.3029.1.5.1).
      const unsigned char OidCurve25519[] = { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x97, 0x55, 0x01, 0x05, 0x01 };

      // The key flags an encryption key carries (RFC 9580 section 5.2.3.29).
      const int FlagEncryptCommunications = 0x04;
      const int FlagEncryptStorage = 0x08;

      unsigned int Read32(const unsigned char *data)
      {
         return ((unsigned int) data[0] << 24) | ((unsigned int) data[1] << 16) |
                ((unsigned int) data[2] << 8) | (unsigned int) data[3];
      }

      unsigned int Read16(const unsigned char *data)
      {
         return ((unsigned int) data[0] << 8) | (unsigned int) data[1];
      }

      AnsiString Hex(const unsigned char *data, size_t length)
      {
         static const char digits[] = "0123456789abcdef";
         AnsiString out;
         for (size_t i = 0; i < length; i++)
         {
            out += digits[data[i] >> 4];
            out += digits[data[i] & 0x0F];
         }
         return out;
      }

      struct Packet
      {
         Packet() : tag(0), body_start(0), body_length(0), next(0) {}
         int tag;
         size_t body_start;
         size_t body_length;
         size_t next;
      };

      // The packet framing of RFC 9580 section 4.2, as OpenPgpKey reads it.
      // The key has been through OpenPgpKey::Parse already, so a refusal here
      // is a defence and not a diagnosis, and its sentence can be short.
      bool ReadPacket(const unsigned char *data, size_t length, size_t offset, Packet &packet)
      {
         if (offset >= length)
            return false;

         const unsigned char first = data[offset];
         if ((first & 0x80) == 0)
            return false;

         size_t body_length = 0;
         size_t body_start = 0;
         if (first & 0x40)
         {
            packet.tag = first & 0x3F;
            if (offset + 2 > length)
               return false;
            const unsigned char o1 = data[offset + 1];
            if (o1 < 192)
            {
               body_length = o1;
               body_start = offset + 2;
            }
            else if (o1 < 224)
            {
               if (offset + 3 > length)
                  return false;
               body_length = (((size_t) o1 - 192) << 8) + (size_t) data[offset + 2] + 192;
               body_start = offset + 3;
            }
            else if (o1 == 255)
            {
               if (offset + 6 > length)
                  return false;
               body_length = Read32(data + offset + 2);
               body_start = offset + 6;
            }
            else
               return false;
         }
         else
         {
            packet.tag = (first >> 2) & 0x0F;
            const int length_type = first & 0x03;
            if (length_type == 0)
            {
               if (offset + 2 > length)
                  return false;
               body_length = data[offset + 1];
               body_start = offset + 2;
            }
            else if (length_type == 1)
            {
               if (offset + 3 > length)
                  return false;
               body_length = Read16(data + offset + 1);
               body_start = offset + 3;
            }
            else if (length_type == 2)
            {
               if (offset + 5 > length)
                  return false;
               body_length = Read32(data + offset + 1);
               body_start = offset + 5;
            }
            else
               return false;
         }

         if (body_start > length || body_length > length - body_start)
            return false;

         packet.body_start = body_start;
         packet.body_length = body_length;
         packet.next = body_start + body_length;
         return true;
      }

      bool NextSubpacket(const unsigned char *data, size_t end, size_t &at, int &type,
                         const unsigned char *&value, size_t &value_length)
      {
         if (at >= end)
            return false;

         size_t length = 0;
         const unsigned char first = data[at];
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
            length = Read32(data + at + 1);
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

      // One key packet - the primary or a subkey - as the chooser sees it.
      struct KeyPacket
      {
         KeyPacket() : version(0), algorithm(0), created(0), expires(0), flags(0), has_flags(false), revoked(false), kdf_hash(0), kdf_cipher(0), usable(false),
                       body_start(0), body_length(0), bound(false) {}

         int version;
         int algorithm;
         __int64 created;
         __int64 expires;
         int flags;
         bool has_flags;
         bool revoked;
         std::vector<unsigned char> fingerprint;
         std::vector<unsigned char> rsa_n;
         std::vector<unsigned char> rsa_e;
         std::vector<unsigned char> point;
         int kdf_hash;
         int kdf_cipher;
         bool usable;      // the material was read and is of a kind this writer encrypts to

         // Where the packet's body is in the key's bytes: a binding signature
         // is made over it.
         size_t body_start;
         size_t body_length;

         // A subkey only: a binding signature that verifies against the
         // primary has been read (or verification was switched off).
         bool bound;
      };

      // An MPI: two octets of bit count, then the octets. Answers the octets
      // without the header and moves at past them.
      bool ReadMpi(const unsigned char *body, size_t length, size_t &at, std::vector<unsigned char> &out)
      {
         if (at + 2 > length)
            return false;
         const unsigned int bits = Read16(body + at);
         const size_t octets = (bits + 7) / 8;
         at += 2;
         if (at + octets > length)
            return false;
         out.assign(body + at, body + at + octets);
         at += octets;
         return true;
      }

      // Reads a version 4 key packet's public material and computes the
      // fingerprint over the whole body, as section 5.5.4 says.
      bool ReadKeyPacket(const unsigned char *data, const Packet &packet, KeyPacket &key)
      {
         const unsigned char *body = data + packet.body_start;
         const size_t length = packet.body_length;
         if (length < 6)
            return false;

         key.body_start = packet.body_start;
         key.body_length = packet.body_length;
         key.version = body[0];
         key.created = (__int64) Read32(body + 1);
         key.algorithm = body[5];
         if (key.version != 4)
            return true;      // read, but never usable: only version 4 is written to

         if (length > 0xFFFF)
            return false;
         std::vector<unsigned char> preimage;
         preimage.push_back(0x99);
         preimage.push_back((unsigned char) ((length >> 8) & 0xFF));
         preimage.push_back((unsigned char) (length & 0xFF));
         preimage.insert(preimage.end(), body, body + length);
         unsigned char digest[SHA_DIGEST_LENGTH] = {};
         SHA1(&preimage[0], preimage.size(), digest);
         key.fingerprint.assign(digest, digest + sizeof(digest));

         size_t at = 6;
         if (key.algorithm == AlgorithmRsa || key.algorithm == AlgorithmRsaEncryptOnly)
         {
            if (!ReadMpi(body, length, at, key.rsa_n) || !ReadMpi(body, length, at, key.rsa_e))
               return false;
            key.usable = key.rsa_n.size() >= 128 && !key.rsa_e.empty();
            return true;
         }

         if (key.algorithm == AlgorithmEcdh)
         {
            if (at >= length)
               return false;
            const size_t oid_length = body[at];
            at += 1;
            if (oid_length == 0 || at + oid_length > length)
               return false;
            const bool curve25519 = oid_length == sizeof(OidCurve25519) && memcmp(body + at, OidCurve25519, oid_length) == 0;
            at += oid_length;
            std::vector<unsigned char> point;
            if (!ReadMpi(body, length, at, point))
               return false;
            if (at >= length)
               return false;
            const size_t kdf_length = body[at];
            at += 1;
            if (kdf_length < 3 || at + kdf_length > length)
               return false;
            key.kdf_hash = body[at + 1];
            key.kdf_cipher = body[at + 2];
            // The MPI of a Curve25519 point is the 0x40 prefix and the 32
            // octets of the u coordinate (RFC 9580 section 5.5.5.6).
            if (curve25519 && point.size() == 33 && point[0] == 0x40)
            {
               key.point.assign(point.begin() + 1, point.end());
               key.usable = true;
            }
            return true;
         }

         return true;
      }

      // What a signature says about the key packet it follows: the key flags
      // and the expiry from the hashed area, and whether it revokes the key.
      // type is answered so that the caller can tell a binding from a
      // certification from a revocation.
      void ReadSignature(const unsigned char *data, const Packet &packet, int &type, bool &has_flags, int &flags, __int64 &expiry)
      {
         type = -1;
         has_flags = false;
         flags = 0;
         expiry = -1;

         const unsigned char *body = data + packet.body_start;
         const size_t length = packet.body_length;
         if (length < 6 || body[0] != 4)
            return;

         type = body[1];
         const size_t hashed_length = Read16(body + 4);
         size_t at = 6;
         if (hashed_length > length - at)
            return;
         const size_t end = at + hashed_length;

         int subpacket_type = 0;
         const unsigned char *value = nullptr;
         size_t value_length = 0;
         while (NextSubpacket(body, end, at, subpacket_type, value, value_length))
         {
            if (subpacket_type == 27 && value_length >= 1)
            {
               has_flags = true;
               flags = value[0];
            }
            else if (subpacket_type == 9 && value_length == 4)
               expiry = (__int64) Read32(value);
         }
      }

      // ---- a subkey's binding to its primary (RFC 9580 section 5.2.4)
      //
      // A subkey binding (0x18) and a subkey revocation (0x28) are made by the
      // primary key over the primary's key packet, the subkey's, and the
      // signature's own hashed area and trailer. Read for their flags and never
      // checked until 24 September 2026, they let anybody who could touch a key
      // on its way here - a keyserver that merges packets unchecked, a mail in
      // transit - append a subkey of their own with a later creation time and a
      // made-up binding under the owner's primary, and so under the fingerprint
      // the owner would read out; or revoke the real subkey with a made-up
      // revocation. GnuPG discards such a subkey, so a key checked there looked
      // right.

      const int AlgorithmRsaSignOnly = 3;
      const int AlgorithmEcdsa = 19;
      const int AlgorithmEddsaLegacy = 22;
      const int AlgorithmEd25519 = 27;

      // The curves a primary key signs on, as their OIDs appear in a key packet.
      const unsigned char OidEd25519[] = { 0x2b, 0x06, 0x01, 0x04, 0x01, 0xda, 0x47, 0x0f, 0x01 };
      const unsigned char OidP256[] = { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 };
      const unsigned char OidP384[] = { 0x2b, 0x81, 0x04, 0x00, 0x22 };
      const unsigned char OidP521[] = { 0x2b, 0x81, 0x04, 0x00, 0x23 };

      // The hash a signature names, or null for one this does not take. SHA-1
      // only for a signature made before 19 January 2019, as GnuPG takes it: a
      // SHA-1 binding made since then is one a collision could have made.
      const EVP_MD *SignatureHashOf(int id, __int64 created)
      {
         switch (id)
         {
         case 2: return created > 0 && created < 1547856000 ? EVP_sha1() : nullptr;
         case 8: return EVP_sha256();
         case 9: return EVP_sha384();
         case 10: return EVP_sha512();
         case 11: return EVP_sha224();
         case 12: return EVP_sha3_256();
         case 14: return EVP_sha3_512();
         default: return nullptr;
         }
      }

      // Owns the primary's verifying key for the length of one walk.
      struct VerifyingKey
      {
         VerifyingKey() : key(nullptr) {}
         ~VerifyingKey()
         {
            if (key)
               EVP_PKEY_free(key);
         }

         EVP_PKEY *key;

      private:
         VerifyingKey(const VerifyingKey &);
         VerifyingKey &operator=(const VerifyingKey &);
      };

      EVP_PKEY *RsaPublicKey(const std::vector<unsigned char> &n, const std::vector<unsigned char> &e)
      {
         if (n.empty() || e.empty())
            return nullptr;

         BIGNUM *modulus = BN_bin2bn(&n[0], (int) n.size(), nullptr);
         BIGNUM *exponent = BN_bin2bn(&e[0], (int) e.size(), nullptr);
         OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
         OSSL_PARAM *params = nullptr;
         EVP_PKEY_CTX *from = nullptr;
         EVP_PKEY *key = nullptr;

         if (modulus && exponent && build &&
             OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, modulus) == 1 &&
             OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, exponent) == 1)
         {
            params = OSSL_PARAM_BLD_to_param(build);
            from = params ? EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr) : nullptr;
            if (from && EVP_PKEY_fromdata_init(from) == 1 && EVP_PKEY_fromdata(from, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
               key = nullptr;
         }

         if (from) EVP_PKEY_CTX_free(from);
         if (params) OSSL_PARAM_free(params);
         if (build) OSSL_PARAM_BLD_free(build);
         if (modulus) BN_free(modulus);
         if (exponent) BN_free(exponent);
         return key;
      }

      EVP_PKEY *EcPublicKey(const char *group, const std::vector<unsigned char> &point)
      {
         if (point.empty())
            return nullptr;

         OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
         OSSL_PARAM *params = nullptr;
         EVP_PKEY_CTX *from = nullptr;
         EVP_PKEY *key = nullptr;

         if (build &&
             OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, group, 0) == 1 &&
             OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, &point[0], point.size()) == 1)
         {
            params = OSSL_PARAM_BLD_to_param(build);
            from = params ? EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr) : nullptr;
            if (from && EVP_PKEY_fromdata_init(from) == 1 && EVP_PKEY_fromdata(from, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
               key = nullptr;
         }

         if (from) EVP_PKEY_CTX_free(from);
         if (params) OSSL_PARAM_free(params);
         if (build) OSSL_PARAM_BLD_free(build);
         return key;
      }

      // The primary key of a version 4 key packet as OpenSSL verifies with it,
      // or null for a kind this cannot check (DSA, a Brainpool curve, v5, v6).
      EVP_PKEY *VerifyingKeyOf(const unsigned char *body, size_t length)
      {
         if (length < 6 || body[0] != 4)
            return nullptr;

         const int algorithm = body[5];
         size_t at = 6;

         if (algorithm == AlgorithmRsa || algorithm == AlgorithmRsaSignOnly)
         {
            std::vector<unsigned char> n;
            std::vector<unsigned char> e;
            if (!ReadMpi(body, length, at, n) || !ReadMpi(body, length, at, e))
               return nullptr;
            return RsaPublicKey(n, e);
         }

         if (algorithm == AlgorithmEd25519)
         {
            if (at + 32 > length)
               return nullptr;
            return EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, body + at, 32);
         }

         if (algorithm != AlgorithmEddsaLegacy && algorithm != AlgorithmEcdsa)
            return nullptr;

         if (at >= length)
            return nullptr;
         const size_t oidLength = body[at];
         at += 1;
         if (oidLength == 0 || oidLength == 0xFF || at + oidLength > length)
            return nullptr;
         const unsigned char *oid = body + at;
         at += oidLength;

         std::vector<unsigned char> point;
         if (!ReadMpi(body, length, at, point))
            return nullptr;

         if (algorithm == AlgorithmEddsaLegacy)
         {
            // The MPI of an Ed25519 point is the 0x40 prefix and 32 octets.
            if (oidLength != sizeof(OidEd25519) || memcmp(oid, OidEd25519, oidLength) != 0 || point.size() != 33 || point[0] != 0x40)
               return nullptr;
            return EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, &point[1], 32);
         }

         const char *group = nullptr;
         if (oidLength == sizeof(OidP256) && memcmp(oid, OidP256, oidLength) == 0)
            group = "prime256v1";
         else if (oidLength == sizeof(OidP384) && memcmp(oid, OidP384, oidLength) == 0)
            group = "secp384r1";
         else if (oidLength == sizeof(OidP521) && memcmp(oid, OidP521, oidLength) == 0)
            group = "secp521r1";

         if (!group || point[0] != 0x04)
            return nullptr;
         return EcPublicKey(group, point);
      }

      // A version 4 signature packet, as far as verifying it needs.
      struct SignatureParts
      {
         SignatureParts() : ok(false), type(-1), algorithm(0), hash(0), created(0), hashed_end(0) {}

         bool ok;
         int type;
         int algorithm;
         int hash;
         __int64 created;
         size_t hashed_end;      // just past the hashed area: what the digest covers of the packet
         std::vector<std::vector<unsigned char> > values;   // the MPIs, or the 64 octets of an Ed25519 signature
      };

      SignatureParts ReadSignatureParts(const unsigned char *body, size_t length)
      {
         SignatureParts parts;
         if (length < 6 || body[0] != 4)
            return parts;

         parts.type = body[1];
         parts.algorithm = body[2];
         parts.hash = body[3];

         const size_t hashedLength = Read16(body + 4);
         if (hashedLength > length - 6)
            return parts;
         parts.hashed_end = 6 + hashedLength;

         size_t at = 6;
         int subpacketType = 0;
         const unsigned char *value = nullptr;
         size_t valueLength = 0;
         while (NextSubpacket(body, parts.hashed_end, at, subpacketType, value, valueLength))
         {
            if (subpacketType == 2 && valueLength == 4)
               parts.created = (__int64) Read32(value);
         }

         at = parts.hashed_end;
         if (at + 2 > length)
            return parts;
         const size_t unhashedLength = Read16(body + at);
         at += 2;
         if (unhashedLength > length - at)
            return parts;
         at += unhashedLength;

         // The left 16 bits of the digest, a quick check this does not need.
         if (at + 2 > length)
            return parts;
         at += 2;

         if (parts.algorithm == AlgorithmEd25519)
         {
            if (at + 64 != length)
               return parts;
            parts.values.push_back(std::vector<unsigned char>(body + at, body + at + 64));
         }
         else
         {
            while (at < length)
            {
               std::vector<unsigned char> mpi;
               if (!ReadMpi(body, length, at, mpi))
                  return parts;
               parts.values.push_back(mpi);
            }
         }

         parts.ok = !parts.values.empty();
         return parts;
      }

      void AppendDerLength(size_t length, std::vector<unsigned char> &out)
      {
         if (length < 0x80)
            out.push_back((unsigned char) length);
         else if (length < 0x100)
         {
            out.push_back(0x81);
            out.push_back((unsigned char) length);
         }
         else
         {
            out.push_back(0x82);
            out.push_back((unsigned char) ((length >> 8) & 0xFF));
            out.push_back((unsigned char) (length & 0xFF));
         }
      }

      void AppendDerInteger(const std::vector<unsigned char> &value, std::vector<unsigned char> &out)
      {
         size_t first = 0;
         while (first + 1 < value.size() && value[first] == 0)
            first++;

         std::vector<unsigned char> content;
         if (value.empty() || (value[first] & 0x80) != 0)
            content.push_back(0);
         if (!value.empty())
            content.insert(content.end(), value.begin() + first, value.end());

         out.push_back(0x02);
         AppendDerLength(content.size(), out);
         out.insert(out.end(), content.begin(), content.end());
      }

      // The ECDSA-Sig-Value OpenSSL verifies: SEQUENCE { INTEGER r, INTEGER s }.
      std::vector<unsigned char> EcdsaSignatureDer(const std::vector<unsigned char> &r, const std::vector<unsigned char> &s)
      {
         std::vector<unsigned char> integers;
         AppendDerInteger(r, integers);
         AppendDerInteger(s, integers);

         std::vector<unsigned char> der;
         der.push_back(0x30);
         AppendDerLength(integers.size(), der);
         der.insert(der.end(), integers.begin(), integers.end());
         return der;
      }

      // The signature over a digest. EdDSA signs the digest itself as its
      // message; RSA and ECDSA sign it as a hash of the named algorithm.
      bool VerifyDigest(EVP_PKEY *key, const SignatureParts &parts, const EVP_MD *md, const unsigned char *digest, unsigned int digestLength)
      {
         const int kind = EVP_PKEY_get_base_id(key);
         bool verified = false;

         if (kind == EVP_PKEY_ED25519)
         {
            std::vector<unsigned char> signature;
            if (parts.algorithm == AlgorithmEd25519 && parts.values.size() == 1 && parts.values[0].size() == 64)
               signature = parts.values[0];
            else if (parts.algorithm == AlgorithmEddsaLegacy && parts.values.size() == 2 &&
                     parts.values[0].size() <= 32 && parts.values[1].size() <= 32)
            {
               // R and S, each an MPI whose leading zero octets are dropped.
               signature.assign(64, 0);
               std::copy(parts.values[0].begin(), parts.values[0].end(), signature.begin() + (32 - parts.values[0].size()));
               std::copy(parts.values[1].begin(), parts.values[1].end(), signature.begin() + (64 - parts.values[1].size()));
            }

            if (!signature.empty())
            {
               EVP_MD_CTX *context = EVP_MD_CTX_new();
               verified = context != nullptr &&
                          EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
                          EVP_DigestVerify(context, &signature[0], signature.size(), digest, digestLength) == 1;
               if (context)
                  EVP_MD_CTX_free(context);
            }
         }
         else if (kind == EVP_PKEY_RSA || kind == EVP_PKEY_EC)
         {
            std::vector<unsigned char> signature;
            if (kind == EVP_PKEY_RSA && (parts.algorithm == AlgorithmRsa || parts.algorithm == AlgorithmRsaSignOnly) && parts.values.size() == 1)
            {
               // OpenSSL takes an RSA signature at the modulus's length, and
               // the MPI has lost its leading zero octets.
               const int size = EVP_PKEY_get_size(key);
               if (size > 0 && parts.values[0].size() <= (size_t) size)
               {
                  signature.assign((size_t) size - parts.values[0].size(), 0);
                  signature.insert(signature.end(), parts.values[0].begin(), parts.values[0].end());
               }
            }
            else if (kind == EVP_PKEY_EC && parts.algorithm == AlgorithmEcdsa && parts.values.size() == 2)
               signature = EcdsaSignatureDer(parts.values[0], parts.values[1]);

            EVP_PKEY_CTX *context = signature.empty() ? nullptr : EVP_PKEY_CTX_new(key, nullptr);
            if (context && EVP_PKEY_verify_init(context) == 1)
            {
               bool ready = true;
               if (kind == EVP_PKEY_RSA)
                  ready = EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) == 1 && EVP_PKEY_CTX_set_signature_md(context, md) == 1;
               verified = ready && EVP_PKEY_verify(context, &signature[0], signature.size(), digest, digestLength) == 1;
            }
            if (context)
               EVP_PKEY_CTX_free(context);
         }

         ERR_clear_error();
         return verified;
      }

      // Whether a signature after a subkey - a binding or a revocation - was
      // made by the primary over the primary and that subkey.
      bool VerifySubkeySignature(EVP_PKEY *primaryKey, const unsigned char *data, const KeyPacket &primary,
                                 const KeyPacket &subkey, const Packet &signature)
      {
         if (!primaryKey || primary.body_length > 0xFFFF || subkey.body_length > 0xFFFF)
            return false;

         const unsigned char *body = data + signature.body_start;
         const SignatureParts parts = ReadSignatureParts(body, signature.body_length);
         if (!parts.ok || (parts.type != 0x18 && parts.type != 0x28))
            return false;

         const EVP_MD *md = SignatureHashOf(parts.hash, parts.created);
         if (!md)
            return false;

         const unsigned char primaryPrefix[3] = { 0x99, (unsigned char) ((primary.body_length >> 8) & 0xFF), (unsigned char) (primary.body_length & 0xFF) };
         const unsigned char subkeyPrefix[3] = { 0x99, (unsigned char) ((subkey.body_length >> 8) & 0xFF), (unsigned char) (subkey.body_length & 0xFF) };
         const size_t hashed = parts.hashed_end;
         const unsigned char trailer[6] = { 0x04, 0xFF,
            (unsigned char) ((hashed >> 24) & 0xFF), (unsigned char) ((hashed >> 16) & 0xFF),
            (unsigned char) ((hashed >> 8) & 0xFF), (unsigned char) (hashed & 0xFF) };

         unsigned char digest[EVP_MAX_MD_SIZE];
         unsigned int digestLength = 0;
         EVP_MD_CTX *hash = EVP_MD_CTX_new();
         const bool computed = hash != nullptr &&
            EVP_DigestInit_ex(hash, md, nullptr) == 1 &&
            EVP_DigestUpdate(hash, primaryPrefix, sizeof(primaryPrefix)) == 1 &&
            EVP_DigestUpdate(hash, data + primary.body_start, primary.body_length) == 1 &&
            EVP_DigestUpdate(hash, subkeyPrefix, sizeof(subkeyPrefix)) == 1 &&
            EVP_DigestUpdate(hash, data + subkey.body_start, subkey.body_length) == 1 &&
            EVP_DigestUpdate(hash, body, hashed) == 1 &&
            EVP_DigestUpdate(hash, trailer, sizeof(trailer)) == 1 &&
            EVP_DigestFinal_ex(hash, digest, &digestLength) == 1;
         if (hash)
            EVP_MD_CTX_free(hash);

         if (!computed)
         {
            ERR_clear_error();
            return false;
         }

         return VerifyDigest(primaryKey, parts, md, digest, digestLength);
      }

      bool Random(std::vector<unsigned char> &out, size_t length, const std::vector<unsigned char> *fixed)
      {
         if (fixed && !fixed->empty())
         {
            out = *fixed;
            return out.size() == length;
         }
         out.assign(length, 0);
         return RAND_bytes(&out[0], (int) length) == 1;
      }

      void WriteMpi(const std::vector<unsigned char> &octets, std::vector<unsigned char> &out)
      {
         size_t first = 0;
         while (first < octets.size() && octets[first] == 0)
            first++;
         unsigned int bits = 0;
         if (first < octets.size())
         {
            bits = (unsigned int) (octets.size() - first - 1) * 8;
            unsigned char lead = octets[first];
            while (lead)
            {
               bits++;
               lead >>= 1;
            }
         }
         out.push_back((unsigned char) ((bits >> 8) & 0xFF));
         out.push_back((unsigned char) (bits & 0xFF));
         out.insert(out.end(), octets.begin() + first, octets.end());
      }

      // The octets a PKESK carries under the public key: the cipher, the key
      // and the two-octet checksum of section 5.1.
      std::vector<unsigned char> SessionKeyMaterial(const std::vector<unsigned char> &sessionKey)
      {
         std::vector<unsigned char> material;
         material.push_back((unsigned char) CipherAes256);
         material.insert(material.end(), sessionKey.begin(), sessionKey.end());
         unsigned int sum = 0;
         for (size_t i = 0; i < sessionKey.size(); i++)
            sum = (sum + sessionKey[i]) & 0xFFFF;
         material.push_back((unsigned char) ((sum >> 8) & 0xFF));
         material.push_back((unsigned char) (sum & 0xFF));
         return material;
      }

      const EVP_MD *HashOf(int id)
      {
         switch (id)
         {
         case 8: return EVP_sha256();
         case 9: return EVP_sha384();
         case 10: return EVP_sha512();
         default: return nullptr;
         }
      }

      // AES-CFB with the whole message as one segment (the CFB-128 mode with
      // no resynchronisation, which is what a version 1 SEIPD packet uses).
      bool CfbEncrypt(const std::vector<unsigned char> &key, const std::vector<unsigned char> &plain, std::vector<unsigned char> &out)
      {
         EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
         if (!context)
            return false;
         unsigned char iv[16] = {};
         out.assign(plain.size() + 16, 0);
         int written = 0;
         int final = 0;
         const bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_cfb128(), nullptr, &key[0], iv) == 1 &&
                         EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
                         EVP_EncryptUpdate(context, &out[0], &written, plain.empty() ? iv : &plain[0], (int) plain.size()) == 1 &&
                         EVP_EncryptFinal_ex(context, &out[0] + written, &final) == 1;
         EVP_CIPHER_CTX_free(context);
         if (!ok)
            return false;
         out.resize((size_t) written + (size_t) final);
         return true;
      }

      bool CfbDecrypt(const std::vector<unsigned char> &key, const std::vector<unsigned char> &cipher, std::vector<unsigned char> &out)
      {
         EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
         if (!context)
            return false;
         unsigned char iv[16] = {};
         out.assign(cipher.size() + 16, 0);
         int written = 0;
         int final = 0;
         const bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_cfb128(), nullptr, &key[0], iv) == 1 &&
                         EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
                         EVP_DecryptUpdate(context, &out[0], &written, cipher.empty() ? iv : &cipher[0], (int) cipher.size()) == 1 &&
                         EVP_DecryptFinal_ex(context, &out[0] + written, &final) == 1;
         EVP_CIPHER_CTX_free(context);
         if (!ok)
            return false;
         out.resize((size_t) written + (size_t) final);
         return true;
      }

      // RFC 3394 with the default initial value, through OpenSSL's wrap mode.
      bool KeyWrap(const std::vector<unsigned char> &kek, const std::vector<unsigned char> &plain, std::vector<unsigned char> &out)
      {
         const EVP_CIPHER *cipher = kek.size() == 16 ? EVP_aes_128_wrap() : kek.size() == 24 ? EVP_aes_192_wrap() : kek.size() == 32 ? EVP_aes_256_wrap() : nullptr;
         if (!cipher || plain.size() % 8 != 0 || plain.size() < 16)
            return false;
         EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
         if (!context)
            return false;
         EVP_CIPHER_CTX_set_flags(context, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
         out.assign(plain.size() + 16, 0);
         int written = 0;
         int final = 0;
         const bool ok = EVP_EncryptInit_ex(context, cipher, nullptr, &kek[0], nullptr) == 1 &&
                         EVP_EncryptUpdate(context, &out[0], &written, &plain[0], (int) plain.size()) == 1 &&
                         EVP_EncryptFinal_ex(context, &out[0] + written, &final) == 1;
         EVP_CIPHER_CTX_free(context);
         if (!ok)
            return false;
         out.resize((size_t) written + (size_t) final);
         return true;
      }

      // The header block of a message and where its body starts: the first
      // empty line, CRLF or bare LF. Answers the length of the header block
      // including the line end of its last field.
      size_t HeaderEnd(const AnsiString &message, size_t &bodyStart)
      {
         const char *text = message.c_str();
         const size_t length = (size_t) message.GetLength();
         for (size_t i = 0; i < length; i++)
         {
            if (text[i] != '\n')
               continue;
            if (i + 2 < length && text[i + 1] == '\r' && text[i + 2] == '\n')
            {
               bodyStart = i + 3;
               return i + 1;
            }
            if (i + 1 < length && text[i + 1] == '\n')
            {
               bodyStart = i + 2;
               return i + 1;
            }
         }
         bodyStart = length;
         return length;
      }

      bool IsContentField(const AnsiString &name)
      {
         AnsiString lowered = name;
         lowered.MakeLower();
         return lowered.StartsWith("content-") || lowered == "mime-version";
      }
   }

   // ---- choosing the key ---------------------------------------------------

   OpenPgpWriter::EncryptionKey
   OpenPgpWriter::ChooseEncryptionKeyBase64(const AnsiString &keyBase64, __int64 now)
   {
      const AnsiString decoded = Base64::Decode(keyBase64.c_str(), keyBase64.GetLength());
      if (decoded.IsEmpty())
      {
         EncryptionKey none;
         none.error = "the public key is not base64 of an OpenPGP key";
         return none;
      }
      return ChooseEncryptionKey((const unsigned char *) decoded.c_str(), (size_t) decoded.GetLength(), now);
   }

   OpenPgpWriter::EncryptionKey
   OpenPgpWriter::ChooseEncryptionKey(const unsigned char *data, size_t length, __int64 now, bool verifyBindings)
   {
      EncryptionKey chosen;

      // The reader the directory publishes through decides what a key IS;
      // this only decides which part of it to encrypt to.
      const OpenPgpKey::Parsed parsed = OpenPgpKey::Parse(data, length, 0);
      if (!parsed.ok)
      {
         chosen.error = parsed.error;
         return chosen;
      }

      KeyPacket primary;
      std::vector<KeyPacket> subkeys;
      bool have_primary = false;

      // The primary as OpenSSL verifies with it; null for a kind this cannot
      // check, and then no subkey binding verifies.
      VerifyingKey primaryKey;

      // A binding that asked for encryption and did not verify: what the
      // refusal says when nothing else could be chosen.
      bool sawUnboundEncryptionSubkey = false;
      // Whose signatures the ones being read belong to: the primary (through
      // its user ids) or the last subkey.
      KeyPacket *current = nullptr;
      bool current_is_primary = false;

      size_t offset = 0;
      while (offset < length)
      {
         Packet packet;
         if (!ReadPacket(data, length, offset, packet))
         {
            chosen.error = "the key's packets could not be walked";
            return chosen;
         }
         offset = packet.next;

         if (packet.tag == TagPublicKey)
         {
            if (have_primary)
               break;
            if (!ReadKeyPacket(data, packet, primary))
            {
               chosen.error = "the key's primary packet could not be read";
               return chosen;
            }
            if (verifyBindings)
               primaryKey.key = VerifyingKeyOf(data + packet.body_start, packet.body_length);
            have_primary = true;
            current = &primary;
            current_is_primary = true;
            continue;
         }

         if (packet.tag == TagUserId)
         {
            current = &primary;
            current_is_primary = true;
            continue;
         }

         if (packet.tag == TagPublicSubkey)
         {
            KeyPacket subkey;
            if (!ReadKeyPacket(data, packet, subkey))
            {
               chosen.error = "a subkey packet could not be read";
               return chosen;
            }
            subkeys.push_back(subkey);
            current = &subkeys.back();
            current_is_primary = false;
            continue;
         }

         if (packet.tag == TagSignature && current)
         {
            int type = -1;
            bool has_flags = false;
            int flags = 0;
            __int64 expiry = -1;
            ReadSignature(data, packet, type, has_flags, flags, expiry);

            if (current_is_primary)
            {
               // A certification of a user id carries the primary's flags and
               // expiry; 0x20 revokes the whole key.
               if (type == 0x20)
                  primary.revoked = true;
               else if (type >= 0x10 && type <= 0x13)
               {
                  if (has_flags)
                  {
                     primary.has_flags = true;
                     primary.flags = flags;
                  }
                  if (expiry >= 0)
                     primary.expires = expiry > 0 ? primary.created + expiry : 0;
               }
            }
            else if (type == 0x18 || type == 0x28)
            {
               // Taken only when the primary made it over this subkey.
               const bool genuine = !verifyBindings || VerifySubkeySignature(primaryKey.key, data, primary, *current, packet);

               if (type == 0x28)
               {
                  if (genuine)
                     current->revoked = true;
               }
               else if (genuine)
               {
                  current->bound = true;
                  if (has_flags)
                  {
                     current->has_flags = true;
                     current->flags = flags;
                  }
                  if (expiry >= 0)
                     current->expires = expiry > 0 ? current->created + expiry : 0;
               }
               else if (has_flags && (flags & (FlagEncryptCommunications | FlagEncryptStorage)) != 0)
                  sawUnboundEncryptionSubkey = true;
            }
         }
      }

      if (!have_primary)
      {
         chosen.error = "the key has no primary key packet";
         return chosen;
      }
      if (primary.revoked)
      {
         chosen.error = "the key is revoked";
         return chosen;
      }
      if (primary.expires && primary.expires < now)
      {
         chosen.error = "the key expired";
         return chosen;
      }

      // The newest living subkey flagged to encrypt, of a kind this writes to.
      const KeyPacket *best = nullptr;
      bool sawEncryptionSubkey = false;
      for (size_t i = 0; i < subkeys.size(); i++)
      {
         const KeyPacket &subkey = subkeys[i];
         if (!subkey.bound || subkey.revoked || !subkey.has_flags || (subkey.flags & (FlagEncryptCommunications | FlagEncryptStorage)) == 0)
            continue;
         if (subkey.expires && subkey.expires < now)
            continue;
         sawEncryptionSubkey = true;
         if (!subkey.usable)
            continue;
         if (!best || subkey.created > best->created)
            best = &subkey;
      }

      if (!best && primary.usable && primary.has_flags && (primary.flags & (FlagEncryptCommunications | FlagEncryptStorage)) != 0 &&
          (primary.algorithm == AlgorithmRsa || primary.algorithm == AlgorithmRsaEncryptOnly))
         best = &primary;

      if (!best)
      {
         if (sawEncryptionSubkey)
            chosen.error = "the key's encryption subkey is of a kind this server does not write to: RSA and Curve25519 (cv25519) are the two it does";
         else if (sawUnboundEncryptionSubkey && !primaryKey.key)
            chosen.error = "the key's primary is of a kind whose signatures this server cannot check (it checks RSA, Ed25519 and ECDSA on the NIST curves), so no encryption subkey can be shown to be bound to it";
         else if (sawUnboundEncryptionSubkey)
            chosen.error = "the key's encryption subkey is not bound to it: its binding signature does not verify against the primary key (or rests on SHA-1 and was made since 2019), so the subkey may not be the key holder's at all. Export the key again from its holder, with gpg --export";
         else
            chosen.error = "the key has no subkey that may encrypt, and its primary key may not either";
         return chosen;
      }

      chosen.ok = true;
      chosen.algorithm = best->algorithm;
      chosen.fingerprint_bytes = best->fingerprint;
      chosen.fingerprint = Hex(&best->fingerprint[0], best->fingerprint.size());
      chosen.primary_fingerprint = Hex(&primary.fingerprint[0], primary.fingerprint.size());
      chosen.key_id.assign(best->fingerprint.end() - 8, best->fingerprint.end());
      chosen.rsa_n = best->rsa_n;
      chosen.rsa_e = best->rsa_e;
      chosen.point = best->point;
      chosen.kdf_hash = best->kdf_hash;
      chosen.kdf_cipher = best->kdf_cipher;
      chosen.created = best->created;
      // When the key stops working: the earlier of the primary's expiry and
      // the chosen subkey's, since this function refuses the key once either
      // has passed. 0 when neither expires - which is what a zero-access
      // mailbox's key has to be (ZeroAccess::Validate says why).
      chosen.expires = best->expires;
      if (primary.expires && (!chosen.expires || primary.expires < chosen.expires))
         chosen.expires = primary.expires;
      return chosen;
   }

   // ---- the packets ---------------------------------------------------------

   void
   OpenPgpWriter::WritePacketHeader(int tag, size_t length, std::vector<unsigned char> &out)
   {
      out.push_back((unsigned char) (0xC0 | (tag & 0x3F)));
      if (length < 192)
         out.push_back((unsigned char) length);
      else if (length < 8384)
      {
         const size_t adjusted = length - 192;
         out.push_back((unsigned char) ((adjusted >> 8) + 192));
         out.push_back((unsigned char) (adjusted & 0xFF));
      }
      else
      {
         out.push_back(0xFF);
         out.push_back((unsigned char) ((length >> 24) & 0xFF));
         out.push_back((unsigned char) ((length >> 16) & 0xFF));
         out.push_back((unsigned char) ((length >> 8) & 0xFF));
         out.push_back((unsigned char) (length & 0xFF));
      }
   }

   bool
   OpenPgpWriter::EncryptSessionKeyRsa_(const EncryptionKey &to, const std::vector<unsigned char> &material,
                                         std::vector<unsigned char> &body, AnsiString &error)
   {
      BIGNUM *n = BN_bin2bn(&to.rsa_n[0], (int) to.rsa_n.size(), nullptr);
      BIGNUM *e = BN_bin2bn(&to.rsa_e[0], (int) to.rsa_e.size(), nullptr);
      OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new();
      OSSL_PARAM *params = nullptr;
      EVP_PKEY *key = nullptr;
      EVP_PKEY_CTX *from = nullptr;
      EVP_PKEY_CTX *encrypt = nullptr;
      std::vector<unsigned char> out;
      bool ok = false;

      do
      {
         if (!n || !e || !build)
            break;
         if (OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n) != 1 ||
             OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e) != 1)
            break;
         params = OSSL_PARAM_BLD_to_param(build);
         if (!params)
            break;
         from = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
         if (!from || EVP_PKEY_fromdata_init(from) != 1 || EVP_PKEY_fromdata(from, &key, EVP_PKEY_PUBLIC_KEY, params) != 1 || !key)
            break;
         encrypt = EVP_PKEY_CTX_new(key, nullptr);
         if (!encrypt || EVP_PKEY_encrypt_init(encrypt) != 1 || EVP_PKEY_CTX_set_rsa_padding(encrypt, RSA_PKCS1_PADDING) != 1)
            break;
         size_t length = 0;
         if (EVP_PKEY_encrypt(encrypt, nullptr, &length, &material[0], material.size()) != 1 || length == 0)
            break;
         out.assign(length, 0);
         if (EVP_PKEY_encrypt(encrypt, &out[0], &length, &material[0], material.size()) != 1)
            break;
         out.resize(length);
         ok = true;
      } while (false);

      if (encrypt) EVP_PKEY_CTX_free(encrypt);
      if (from) EVP_PKEY_CTX_free(from);
      if (key) EVP_PKEY_free(key);
      if (params) OSSL_PARAM_free(params);
      if (build) OSSL_PARAM_BLD_free(build);
      if (n) BN_free(n);
      if (e) BN_free(e);

      if (!ok)
      {
         error = "the session key could not be encrypted to the RSA key";
         return false;
      }

      body.push_back(3);
      body.insert(body.end(), to.key_id.begin(), to.key_id.end());
      // The recipient key's own algorithm octet (RFC 9580 section 5.1): 1 for
      // an RSA key, 2 for the old encrypt-only kind, which the chooser accepts
      // too and which GnuPG matches a session key packet to by id AND algorithm.
      body.push_back((unsigned char) to.algorithm);
      WriteMpi(out, body);
      return true;
   }

   bool
   OpenPgpWriter::EncryptSessionKeyEcdh_(const EncryptionKey &to, const std::vector<unsigned char> &material,
                                          const std::vector<unsigned char> &scalar, std::vector<unsigned char> &body, AnsiString &error)
   {
      const EVP_MD *hash = HashOf(to.kdf_hash);
      const size_t kekLength = to.kdf_cipher == 7 ? 16 : to.kdf_cipher == 8 ? 24 : to.kdf_cipher == 9 ? 32 : 0;
      if (!hash || kekLength == 0)
      {
         error = "the key's ECDH parameters name a hash or a cipher this server does not have";
         return false;
      }

      EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, &to.point[0], to.point.size());
      EVP_PKEY *ephemeral = nullptr;
      EVP_PKEY_CTX *generate = nullptr;
      EVP_PKEY_CTX *derive = nullptr;
      std::vector<unsigned char> shared(32, 0);
      unsigned char ephemeralPoint[32] = {};
      bool ok = false;

      do
      {
         if (!peer)
            break;
         if (!scalar.empty())
            ephemeral = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, &scalar[0], scalar.size());
         else
         {
            generate = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
            if (!generate || EVP_PKEY_keygen_init(generate) != 1 || EVP_PKEY_keygen(generate, &ephemeral) != 1)
               break;
         }
         if (!ephemeral)
            break;
         size_t pointLength = sizeof(ephemeralPoint);
         if (EVP_PKEY_get_raw_public_key(ephemeral, ephemeralPoint, &pointLength) != 1 || pointLength != 32)
            break;
         derive = EVP_PKEY_CTX_new(ephemeral, nullptr);
         size_t sharedLength = shared.size();
         if (!derive || EVP_PKEY_derive_init(derive) != 1 || EVP_PKEY_derive_set_peer(derive, peer) != 1 ||
             EVP_PKEY_derive(derive, &shared[0], &sharedLength) != 1 || sharedLength != 32)
            break;
         ok = true;
      } while (false);

      if (derive) EVP_PKEY_CTX_free(derive);
      if (generate) EVP_PKEY_CTX_free(generate);
      if (ephemeral) EVP_PKEY_free(ephemeral);
      if (peer) EVP_PKEY_free(peer);

      if (!ok)
      {
         error = "the X25519 agreement with the key's point failed";
         return false;
      }

      // RFC 6637 section 7, the KDF: a counter, the shared point, then the
      // parameters that bind the result to this key and this algorithm.
      std::vector<unsigned char> input;
      input.push_back(0); input.push_back(0); input.push_back(0); input.push_back(1);
      input.insert(input.end(), shared.begin(), shared.end());
      input.push_back((unsigned char) sizeof(OidCurve25519));
      input.insert(input.end(), OidCurve25519, OidCurve25519 + sizeof(OidCurve25519));
      input.push_back((unsigned char) AlgorithmEcdh);
      input.push_back(3);
      input.push_back(1);
      input.push_back((unsigned char) to.kdf_hash);
      input.push_back((unsigned char) to.kdf_cipher);
      static const char anonymousSender[] = "Anonymous Sender    ";
      input.insert(input.end(), anonymousSender, anonymousSender + 20);
      input.insert(input.end(), to.fingerprint_bytes.begin(), to.fingerprint_bytes.end());

      unsigned char digest[EVP_MAX_MD_SIZE] = {};
      unsigned int digestLength = 0;
      if (EVP_Digest(&input[0], input.size(), digest, &digestLength, hash, nullptr) != 1 || digestLength < kekLength)
      {
         error = "the ECDH key derivation failed";
         return false;
      }
      std::vector<unsigned char> kek(digest, digest + kekLength);

      // The material padded to a multiple of eight with the count of padding
      // octets, as section 8 of RFC 6637 says, then wrapped.
      std::vector<unsigned char> padded = material;
      const size_t padding = 8 - (padded.size() % 8);
      for (size_t i = 0; i < padding; i++)
         padded.push_back((unsigned char) padding);

      std::vector<unsigned char> wrapped;
      if (!KeyWrap(kek, padded, wrapped) || wrapped.size() > 255)
      {
         error = "the session key could not be wrapped";
         return false;
      }

      body.push_back(3);
      body.insert(body.end(), to.key_id.begin(), to.key_id.end());
      body.push_back((unsigned char) AlgorithmEcdh);
      std::vector<unsigned char> point;
      point.push_back(0x40);
      point.insert(point.end(), ephemeralPoint, ephemeralPoint + 32);
      WriteMpi(point, body);
      body.push_back((unsigned char) wrapped.size());
      body.insert(body.end(), wrapped.begin(), wrapped.end());
      return true;
   }

   bool
   OpenPgpWriter::Encrypt(const EncryptionKey &to, const unsigned char *data, size_t length,
                          std::vector<unsigned char> &message, AnsiString &error, const Randomness *fixed)
   {
      message.clear();
      if (!to.ok)
      {
         error = to.error.IsEmpty() ? AnsiString("no key to encrypt to") : to.error;
         return false;
      }

      std::vector<unsigned char> sessionKey;
      std::vector<unsigned char> prefix;
      std::vector<unsigned char> scalar;
      if (!Random(sessionKey, 32, fixed ? &fixed->session_key : nullptr) || !Random(prefix, 16, fixed ? &fixed->prefix : nullptr))
      {
         error = "no random bytes could be drawn for the session key";
         return false;
      }
      if (fixed && !fixed->ephemeral_scalar.empty())
         scalar = fixed->ephemeral_scalar;

      // The session key packet.
      std::vector<unsigned char> pkesk;
      const std::vector<unsigned char> material = SessionKeyMaterial(sessionKey);
      if (to.algorithm == AlgorithmRsa || to.algorithm == AlgorithmRsaEncryptOnly)
      {
         if (!EncryptSessionKeyRsa_(to, material, pkesk, error))
            return false;
      }
      else if (to.algorithm == AlgorithmEcdh)
      {
         if (!EncryptSessionKeyEcdh_(to, material, scalar, pkesk, error))
            return false;
      }
      else
      {
         error = "the key's algorithm is not one this server writes to";
         return false;
      }

      // What is encrypted: the prefix with its last two octets repeated, the
      // literal packet (binary, no name, no date), and the MDC packet over all
      // of it - written into one buffer, so that the message is held once
      // more here and not twice (the gateway encrypts messages of up to 50 MB).
      std::vector<unsigned char> plain;
      plain.reserve(18 + 6 + 6 + length + 22);
      plain.insert(plain.end(), prefix.begin(), prefix.end());
      plain.push_back(prefix[14]);
      plain.push_back(prefix[15]);
      WritePacketHeader(TagLiteral, length + 6, plain);
      plain.push_back('b');
      plain.push_back(0);
      plain.push_back(0); plain.push_back(0); plain.push_back(0); plain.push_back(0);
      plain.insert(plain.end(), data, data + length);
      plain.push_back(0xD3);
      plain.push_back(0x14);
      unsigned char mdc[SHA_DIGEST_LENGTH] = {};
      SHA1(&plain[0], plain.size(), mdc);
      plain.insert(plain.end(), mdc, mdc + sizeof(mdc));

      std::vector<unsigned char> encrypted;
      const bool encryptedOk = CfbEncrypt(sessionKey, plain, encrypted);

      // The key and the plaintext are not left in memory longer than they
      // are needed; a best effort, and said so. The plaintext's buffer is
      // let go of before the packets are written.
      std::fill(sessionKey.begin(), sessionKey.end(), 0);
      std::fill(plain.begin(), plain.end(), 0);
      std::vector<unsigned char>().swap(plain);

      if (!encryptedOk)
      {
         error = "the message could not be encrypted";
         return false;
      }

      message.reserve(pkesk.size() + 6 + encrypted.size() + 1 + 6);
      WritePacketHeader(TagPkesk, pkesk.size(), message);
      message.insert(message.end(), pkesk.begin(), pkesk.end());
      WritePacketHeader(TagSeipd, encrypted.size() + 1, message);
      message.push_back(1);
      message.insert(message.end(), encrypted.begin(), encrypted.end());
      return true;
   }

   AnsiString
   OpenPgpWriter::Armour(const std::vector<unsigned char> &message)
   {
      AnsiString out = "-----BEGIN PGP MESSAGE-----\r\n\r\n";
      out.reserve(message.size() / 57 * 78 + 256);

      // Encoded a block at a time, 1,024 lines of 57 octets: a multiple of
      // three, so the blocks' base64 joined is the whole message's, and of 57,
      // so every line is whole. The whole message's base64 is never held
      // beside the armour.
      const size_t block = 57 * 1024;
      for (size_t start = 0; start < message.size(); start += block)
      {
         const size_t count = message.size() - start < block ? message.size() - start : block;
         const AnsiString base64 = Base64::Encode((const char *) &message[start], (int) count);
         const size_t encoded = (size_t) base64.GetLength();
         for (size_t at = 0; at < encoded; at += 76)
         {
            out.append(base64.c_str() + at, encoded - at < 76 ? encoded - at : 76);
            out += "\r\n";
         }
      }
      const unsigned int crc = message.empty() ? 0x00B704CEu : OpenPgpKey::ArmourChecksum(&message[0], message.size());
      const unsigned char crcOctets[3] = { (unsigned char) ((crc >> 16) & 0xFF), (unsigned char) ((crc >> 8) & 0xFF), (unsigned char) (crc & 0xFF) };
      out += "=";
      out += Base64::Encode((const char *) crcOctets, 3);
      out += "\r\n-----END PGP MESSAGE-----\r\n";
      return out;
   }

   bool
   OpenPgpWriter::IsPgpMimeEncrypted(const AnsiString &message)
   {
      size_t bodyStart = 0;
      const size_t headerLength = HeaderEnd(message, bodyStart);
      AnsiString header = message.Mid(0, (int) headerLength);
      header.MakeLower();
      // Folded lines joined, so a Content-Type split over two lines reads as one.
      header.Replace("\r\n ", " ");
      header.Replace("\r\n\t", " ");
      header.Replace("\n ", " ");
      header.Replace("\n\t", " ");
      // The field at the start of a line, so that a name that merely ends in
      // it (X-Original-Content-Type) is not read as the message's own.
      int at = header.StartsWith("content-type:") ? 0 : header.Find("\ncontent-type:");
      if (at < 0)
         return false;
      if (at > 0)
         at += 1;
      // Only the first Content-Type field, at the top level.
      const int lineEnd = header.Find("\n", at);
      const AnsiString field = lineEnd < 0 ? header.Mid(at) : header.Mid(at, lineEnd - at);
      return field.Find("multipart/encrypted") >= 0 && field.Find("application/pgp-encrypted") >= 0;
   }

   bool
   OpenPgpWriter::SealMessage(const EncryptionKey &to, const AnsiString &original,
                              AnsiString &sealed, AnsiString &error, const Randomness *fixed)
   {
      sealed.Empty();

      std::vector<unsigned char> message;
      if (!Encrypt(to, (const unsigned char *) original.c_str(), (size_t) original.GetLength(), message, error, fixed))
         return false;

      // The outer headers: the original's, less what described the body that
      // is no longer there. A field is one line and every folded line after it.
      size_t bodyStart = 0;
      const size_t headerLength = HeaderEnd(original, bodyStart);
      AnsiString outer;
      {
         const AnsiString header = original.Mid(0, (int) headerLength);
         const char *text = header.c_str();
         const size_t length = (size_t) header.GetLength();
         size_t at = 0;
         while (at < length)
         {
            // One field: up to a line end that is not followed by white space.
            size_t end = at;
            while (end < length)
            {
               size_t lineEnd = end;
               while (lineEnd < length && text[lineEnd] != '\n')
                  lineEnd++;
               end = lineEnd < length ? lineEnd + 1 : length;
               if (end < length && (text[end] == ' ' || text[end] == '\t'))
                  continue;
               break;
            }
            const AnsiString field(std::string(text + at, end - at));
            at = end;
            const int colon = field.Find(":");
            if (colon <= 0)
               continue;
            const AnsiString name = field.Mid(0, colon);
            if (IsContentField(name))
               continue;
            // CRLF line ends throughout, whatever the file had.
            AnsiString normalised = field;
            normalised.Replace("\r\n", "\n");
            normalised.Replace("\n", "\r\n");
            if (!normalised.EndsWith("\r\n"))
               normalised += "\r\n";
            outer += normalised;
         }
      }

      unsigned char boundaryBytes[16] = {};
      if (fixed)
         memset(boundaryBytes, 0x5A, sizeof(boundaryBytes));
      else if (RAND_bytes(boundaryBytes, sizeof(boundaryBytes)) != 1)
      {
         error = "no random bytes could be drawn for the boundary";
         return false;
      }
      const AnsiString boundary = "hmailserver-zero-access-" + Hex(boundaryBytes, sizeof(boundaryBytes));

      sealed = outer;
      // The server's own mark, so that the webmail can say "sealed on
      // delivery" rather than "an OpenPGP message", and a test can find the
      // shape by name. It says nothing a reader of the headers could not see
      // from the Content-Type below it.
      sealed += "X-hMailServer-Zero-Access: sealed on delivery\r\n";
      sealed += "MIME-Version: 1.0\r\n";
      sealed += "Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";\r\n boundary=\"" + boundary + "\"\r\n";
      sealed += "\r\n";
      sealed += "This is an OpenPGP/MIME encrypted message (RFC 3156). It was encrypted on delivery to the mailbox's own key, and only the mailbox's key opens it.\r\n";
      sealed += "\r\n--" + boundary + "\r\n";
      sealed += "Content-Type: application/pgp-encrypted\r\nContent-Description: PGP/MIME version identification\r\n\r\nVersion: 1\r\n";
      sealed += "\r\n--" + boundary + "\r\n";
      sealed += "Content-Type: application/octet-stream; name=\"encrypted.asc\"\r\nContent-Description: OpenPGP encrypted message\r\nContent-Disposition: inline; filename=\"encrypted.asc\"\r\n\r\n";
      sealed += Armour(message);
      sealed += "\r\n--" + boundary + "--\r\n";
      return true;
   }

   // ---------------------------------------------------------------------------
   // The tests. Every input is a value; the randomness is fixed through the hook
   // so that the packets can be compared to numbers computed outside this file,
   // and then the message is opened again with the session key the test chose.
   //
   // WHERE THE VECTORS COME FROM. The two keys are the fixtures GnuPG 2.4 made
   // for build/check-portal-openpgp.js (build/fixtures/openpgp/alice.pub.asc,
   // Ed25519 with a cv25519 subkey, and bob.pub.asc, RSA 3072); the
   // fingerprints asserted are the ones gpg printed for them. The X25519
   // agreement is checked against RFC 7748 section 6.1's test vector before
   // any key is touched, so the arithmetic the KDF stands on is proved by the
   // RFC and not by this file agreeing with itself.
   // ---------------------------------------------------------------------------

   namespace
   {
      void Require(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      int HexValue(char c)
      {
         if (c >= '0' && c <= '9') return c - '0';
         if (c >= 'a' && c <= 'f') return c - 'a' + 10;
         if (c >= 'A' && c <= 'F') return c - 'A' + 10;
         return 0;
      }

      std::vector<unsigned char> FromHex(const char *text)
      {
         std::vector<unsigned char> out;
         for (size_t i = 0; text[i] && text[i + 1]; i += 2)
            out.push_back((unsigned char) ((HexValue(text[i]) << 4) | HexValue(text[i + 1])));
         return out;
      }

      // alice@fixtures.test: the Ed25519 primary 3C6F F5E3 4581 73DD 5DB3 F4A4
      // A142 63C9 7DE6 857D with the cv25519 subkey EAA03821629D0473 - the id
      // gpg names when it decrypts encrypted-to-alice-plain.asc. The armour is
      // build/fixtures/openpgp/alice.pub.asc byte for byte.
      const char *AliceArmoured =
         "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n"
         "mDMEaq0hphYJKwYBBAHaRw8BAQdAogB/YdOhOFS5ajaz62eLUepoNXY/3nTQgUXE\n"
         "lQCO+fy0I0FsaWNlIEZpeHR1cmUgPGFsaWNlQGZpeHR1cmVzLnRlc3Q+iJAEExYK\n"
         "ADgWIQQ8b/XjRYFz3V2z9KShQmPJfeaFfQUCaq0hpgIbAwULCQgHAgYVCgkICwIE\n"
         "FgIDAQIeAQIXgAAKCRChQmPJfeaFfaSaAP4z1aPIosKBxbw2o6g4StHegDSHti6i\n"
         "6BaakoCP3DqqoAEAvvu1HHt590Ku50WUoHSMaO9EdiPdNpDFlOZv3UVyfAi4OARq\n"
         "rSGmEgorBgEEAZdVAQUBAQdAGQu/CHDJ97RxQz9zk4Jk8BUAvFvLbNRtnTzZll8C\n"
         "QzEDAQgHiHgEGBYKACAWIQQ8b/XjRYFz3V2z9KShQmPJfeaFfQUCaq0hpgIbDAAK\n"
         "CRChQmPJfeaFfTkvAP9vpnBYjBI3tYEJqpJteokGrh4/EWyM3WNO/DUJtc62aAEA\n"
         "ukeUO3aWgpkHejO2t5W84vVO3MeX4UFOng31q2kkOA0=\n"
         "=gUJB\n"
         "-----END PGP PUBLIC KEY BLOCK-----\n";

      // revsub@fixtures.test: GnuPG 2.4.7 made it on 24 September 2026 - an
      // Ed25519 primary 10E3 6BDC D1BA ED46 3775 2D94 1458 2CAD F579 2BD6 and
      // one cv25519 subkey 96407CDB257335C493BDCE0D51D0A08FDE566029, which
      // "gpg --edit-key", "key 1", "revkey" then revoked. gpg --export, base64.
      const char *RevokedSubkeyBase64 =
         "mDMEarSd/hYJKwYBBAHaRw8BAQdAI2EvJ6+NFcgnZ47VE87sv7CWmVMx5pubKibY"
         "udXSTqC0JVJldm9rZWQgU3Via2V5IDxyZXZzdWJAZml4dHVyZXMudGVzdD6IkwQT"
         "FgoAOxYhBBDja9zRuu1GN3UtlBRYLK31eSvWBQJqtJ3+AhsDBQsJCAcCAiICBhUK"
         "CQgLAgQWAgMBAh4HAheAAAoJEBRYLK31eSvWVPYBAMWVuiu72u9bgnfwPrPfhNoY"
         "udR9G8RWVCnCUFwq7dyQAP47oEdKvgsuKid0UEOFkPaJhQw1w5Nz+T4xf8A6rKJb"
         "Drg4BGq0nf8SCisGAQQBl1UBBQEBB0A/GFCb8Bj8NkbBJagdzAkEuvQSPmpYWVHs"
         "8IL/o/VLNwMBCAeIeAQoFgoAIBYhBBDja9zRuu1GN3UtlBRYLK31eSvWBQJqtJ3/"
         "Ah0AAAoJEBRYLK31eSvWLEkBAJs3raVopOpPAbFw8Ncy+vNTuIZhxsQgy1YUosgY"
         "3afNAQDfPVI58yB65ibBJcJpBqLwbaAE52Uth+3+BU6nHmC1BYh4BBgWCgAgFiEE"
         "EONr3NG67UY3dS2UFFgsrfV5K9YFAmq0nf8CGwwACgkQFFgsrfV5K9bTPAD6Ao8q"
         "4gYhTNwphhWLu4BaiGDiVnkaG7D71YNfiLdIsqoA/2ZjrZPKTCS0FO6X7oaGu8MF"
         "JbrzHkSL6rbYUEv817wB";
   }

   namespace
   {
      // The creation time in the first packet of the given tag (6 for the
      // primary, 14 for the subkey): the four octets after the version.
      __int64 CreationTimeOf(const std::vector<unsigned char> &key, int tag)
      {
         size_t offset = 0;
         Packet packet;
         while (ReadPacket(&key[0], key.size(), offset, packet))
         {
            if (packet.tag == tag && packet.body_length >= 5)
               return (__int64) Read32(&key[packet.body_start + 1]);
            offset = packet.next;
         }
         return 0;
      }

      // The key with a key-expiry subpacket (RFC 9580 section 5.2.3.13) put
      // at the front of the hashed area of the signature that follows the
      // first packet of the given tag - the binding after the subkey (14),
      // the self-certification after the user id (13). Nothing is re-signed:
      // a changed binding no longer verifies, so the chooser refuses the
      // subkey it binds (the tests below say so), and the expiry arithmetic
      // is tested with verification switched off. The self-certification is
      // read unverified. Empty when the packet was not found.
      std::vector<unsigned char> WithExpiry(const std::vector<unsigned char> &key, int afterTag, unsigned int seconds)
      {
         size_t offset = 0;
         Packet packet;
         bool found = false;
         while (ReadPacket(&key[0], key.size(), offset, packet))
         {
            if (found && packet.tag == 2)
               break;
            found = found || packet.tag == afterTag;
            offset = packet.next;
         }
         if (!found || packet.tag != 2 || packet.body_length < 8 || key[packet.body_start] != 4)
            return std::vector<unsigned char>();

         std::vector<unsigned char> body(key.begin() + packet.body_start, key.begin() + packet.body_start + packet.body_length);
         const size_t hashedLength = ((size_t) body[4] << 8) | body[5];
         if (6 + hashedLength > body.size())
            return std::vector<unsigned char>();
         unsigned char subpacket[6] = { 5, 9,
            (unsigned char) ((seconds >> 24) & 0xFF), (unsigned char) ((seconds >> 16) & 0xFF),
            (unsigned char) ((seconds >> 8) & 0xFF), (unsigned char) (seconds & 0xFF) };
         body.insert(body.begin() + 6, subpacket, subpacket + 6);
         const size_t newHashed = hashedLength + 6;
         body[4] = (unsigned char) ((newHashed >> 8) & 0xFF);
         body[5] = (unsigned char) (newHashed & 0xFF);

         std::vector<unsigned char> out(key.begin(), key.begin() + offset);
         OpenPgpWriter::WritePacketHeader(2, body.size(), out);
         out.insert(out.end(), body.begin(), body.end());
         out.insert(out.end(), key.begin() + packet.next, key.end());
         return out;
      }
   }

   void
   OpenPgpWriterTester::Test()
   {
      // ---- the arithmetic: RFC 7748 section 6.1
      {
         const std::vector<unsigned char> alicePrivate = FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
         const std::vector<unsigned char> bobPublic = FromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
         const std::vector<unsigned char> expected = FromHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

         EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, &alicePrivate[0], 32);
         EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, &bobPublic[0], 32);
         EVP_PKEY_CTX *derive = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;
         std::vector<unsigned char> shared(32, 0);
         size_t length = 32;
         const bool ok = derive && theirs && EVP_PKEY_derive_init(derive) == 1 && EVP_PKEY_derive_set_peer(derive, theirs) == 1 &&
                         EVP_PKEY_derive(derive, &shared[0], &length) == 1;
         if (derive) EVP_PKEY_CTX_free(derive);
         if (mine) EVP_PKEY_free(mine);
         if (theirs) EVP_PKEY_free(theirs);
         Require(ok && shared == expected, "X25519 does not reproduce RFC 7748's shared secret.");
      }

      // ---- the packet header
      {
         std::vector<unsigned char> h;
         OpenPgpWriter::WritePacketHeader(11, 5, h);
         Require(h.size() == 2 && h[0] == 0xCB && h[1] == 5, "A one-octet packet length is not written as RFC 9580 says.");
         h.clear();
         OpenPgpWriter::WritePacketHeader(18, 1000, h);
         Require(h.size() == 3 && h[0] == 0xD2 && h[1] == 195 && h[2] == 40, "A two-octet packet length is not written as RFC 9580 says.");
         h.clear();
         OpenPgpWriter::WritePacketHeader(1, 100000, h);
         Require(h.size() == 6 && h[0] == 0xC1 && h[1] == 0xFF && h[2] == 0 && h[3] == 1 && h[4] == 0x86 && h[5] == 0xA0, "A five-octet packet length is not written as RFC 9580 says.");
      }

      // ---- the key GnuPG made, read for the subkey to encrypt to
      std::vector<unsigned char> aliceBytes;
      {
         AnsiString problem;
         Require(OpenPgpKey::Dearmour(AnsiString(AliceArmoured), aliceBytes, problem), "The fixture key's armour did not open.");
      }
      const OpenPgpWriter::EncryptionKey alice = OpenPgpWriter::ChooseEncryptionKey(&aliceBytes[0], aliceBytes.size(), 1800000000);
      Require(alice.ok, alice.error.IsEmpty() ? "The fixture key offered nothing to encrypt to." : alice.error.c_str());
      Require(alice.algorithm == 18, "The fixture's encryption key is not its ECDH subkey.");
      Require(alice.primary_fingerprint == "3c6ff5e3458173dd5db3f4a4a14263c97de6857d", "The fixture key's primary fingerprint is not the one gpg prints.");
      Require(Hex(&alice.key_id[0], 8) == "eaa03821629d0473", "The subkey chosen is not the one gpg encrypts to (EAA03821629D0473).");
      Require(alice.point.size() == 32 && alice.kdf_hash == 8 && alice.kdf_cipher == 7, "The cv25519 subkey's point or KDF parameters were not read.");

      // ---- a message to it, with the randomness fixed, opened again here
      const char *text = "A message the server sealed on delivery.\r\n";
      OpenPgpWriter::Randomness fixed;
      fixed.session_key = FromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
      fixed.prefix = FromHex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
      fixed.ephemeral_scalar = FromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
      std::vector<unsigned char> message;
      AnsiString error;
      Require(OpenPgpWriter::Encrypt(alice, (const unsigned char *) text, strlen(text), message, error, &fixed), error.IsEmpty() ? "The message was not written." : error.c_str());

      // The PKESK: version 3, the subkey's id, ECDH, the ephemeral point as an
      // MPI of 263 bits, then 48 wrapped octets (40 padded octets plus 8).
      {
         Packet pkesk;
         Require(ReadPacket(&message[0], message.size(), 0, pkesk) && pkesk.tag == 1, "The message does not begin with a session key packet.");
         const unsigned char *body = &message[pkesk.body_start];
         Require(body[0] == 3, "The session key packet is not version 3.");
         Require(Hex(body + 1, 8) == "eaa03821629d0473", "The session key packet does not name the subkey.");
         Require(body[9] == 18, "The session key packet does not say ECDH.");
         Require(body[10] == 0x01 && body[11] == 0x07 && body[12] == 0x40, "The ephemeral point is not a 263-bit MPI beginning 0x40.");
         // RFC 7748's own public key for that scalar, since the scalar is its.
         Require(Hex(body + 13, 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", "The ephemeral point is not the public key of the fixed scalar.");
         Require(body[45] == 48 && pkesk.body_length == 46 + 48, "The wrapped session key is not 48 octets.");

         // The SEIPD: version 1, and the ciphertext of prefix + literal + MDC.
         Packet seipd;
         Require(ReadPacket(&message[0], message.size(), pkesk.next, seipd) && seipd.tag == 18, "The session key packet is not followed by the encrypted data packet.");
         Require(seipd.next == message.size(), "There are bytes after the encrypted data packet.");
         const unsigned char *data = &message[seipd.body_start];
         Require(data[0] == 1, "The encrypted data packet is not version 1.");
         std::vector<unsigned char> cipher(data + 1, data + seipd.body_length);
         std::vector<unsigned char> plain;
         Require(CfbDecrypt(fixed.session_key, cipher, plain), "The encrypted data did not decrypt with the session key.");
         Require(plain.size() > 18 + 22 && std::equal(fixed.prefix.begin(), fixed.prefix.end(), plain.begin()) && plain[16] == plain[14] && plain[17] == plain[15],
                 "The decrypted data does not begin with the prefix and its repeated octets.");
         Packet literal;
         Require(ReadPacket(&plain[0], plain.size(), 18, literal) && literal.tag == 11, "The prefix is not followed by a literal data packet.");
         const unsigned char *lit = &plain[literal.body_start];
         Require(lit[0] == 'b' && lit[1] == 0 && literal.body_length == 6 + strlen(text) && memcmp(lit + 6, text, strlen(text)) == 0,
                 "The literal packet does not carry the message as binary with no name.");
         Require(literal.next + 22 == plain.size() && plain[literal.next] == 0xD3 && plain[literal.next + 1] == 0x14, "The literal packet is not followed by the MDC packet.");
         unsigned char mdc[SHA_DIGEST_LENGTH] = {};
         SHA1(&plain[0], literal.next + 2, mdc);
         Require(memcmp(mdc, &plain[literal.next + 2], 20) == 0, "The MDC is not the SHA-1 of what precedes it.");
      }

      // ---- the same message twice with fresh randomness differs
      {
         std::vector<unsigned char> one, two;
         Require(OpenPgpWriter::Encrypt(alice, (const unsigned char *) text, strlen(text), one, error) &&
                 OpenPgpWriter::Encrypt(alice, (const unsigned char *) text, strlen(text), two, error) && one != two,
                 "Two encryptions of one message came out the same, so the randomness is not random.");
      }

      // ---- the armour round-trips through the reader's dearmour
      {
         const AnsiString armoured = OpenPgpWriter::Armour(message);
         Require(armoured.StartsWith("-----BEGIN PGP MESSAGE-----\r\n\r\n") && armoured.EndsWith("-----END PGP MESSAGE-----\r\n"), "The armour's frame is wrong.");
         std::vector<unsigned char> back;
         AnsiString problem;
         // Dearmour is the public key's door and reads that label only; a
         // message block has its own.
         Require(OpenPgpKey::DearmourMessage(armoured, back, problem) && back == message, "The armoured message does not dearmour to the same bytes.");
      }

      // ---- PGP/MIME: the outer headers kept, the Content-* fields gone
      {
         const AnsiString original =
            "Return-Path: <sender@example.org>\r\n"
            "Received: from somewhere\r\n by here; Thu, 18 Sep 2026 10:00:00 +0000\r\n"
            "From: sender@example.org\r\n"
            "To: alice@fixtures.test\r\n"
            "Subject: The subject stays\r\n"
            "Message-ID: <one@example.org>\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Transfer-Encoding: 8bit\r\n"
            "\r\n"
            "The body goes under the key.\r\n";
         AnsiString sealed;
         Require(OpenPgpWriter::SealMessage(alice, original, sealed, error, &fixed), "The message was not sealed.");
         Require(sealed.Find("Subject: The subject stays\r\n") >= 0 && sealed.Find("Message-ID: <one@example.org>\r\n") >= 0 &&
                 sealed.Find("Received: from somewhere\r\n by here") >= 0, "The kept headers are not all there.");
         Require(sealed.Find("text/plain") < 0 && sealed.Find("Content-Transfer-Encoding: 8bit") < 0, "The original's Content-* fields were kept.");
         Require(sealed.Find("The body goes under the key.") < 0, "The plaintext body is in the sealed message.");
         Require(sealed.Find("Content-Type: multipart/encrypted; protocol=\"application/pgp-encrypted\";") >= 0 &&
                 sealed.Find("Content-Type: application/pgp-encrypted") >= 0 && sealed.Find("Version: 1\r\n") >= 0 &&
                 sealed.Find("-----BEGIN PGP MESSAGE-----") >= 0, "The sealed message is not shaped as RFC 3156 says.");
         Require(OpenPgpWriter::IsPgpMimeEncrypted(sealed), "The sealed message is not recognised as PGP/MIME.");
         Require(sealed.Find("X-hMailServer-Zero-Access: sealed on delivery\r\n") >= 0, "The sealed message does not carry the server's mark.");
         Require(!OpenPgpWriter::IsPgpMimeEncrypted(original), "A plain message is taken for PGP/MIME.");
         {
            const int first = sealed.Find("MIME-Version:");
            Require(first >= 0 && sealed.Find("MIME-Version:", first + 1) < 0, "MIME-Version does not appear exactly once in the sealed message.");
         }
      }

      // ---- what is refused: a revoked key, and a key with nothing to encrypt to
      {
         // Not a key at all.
         const unsigned char junk[] = { 0x01, 0x02, 0x03 };
         Require(!OpenPgpWriter::ChooseEncryptionKey(junk, sizeof(junk), 1800000000).ok, "Three junk bytes were taken for a key.");
         // The primary alone, without its subkey: an Ed25519 key that cannot encrypt.
         Packet primary;
         Require(ReadPacket(&aliceBytes[0], aliceBytes.size(), 0, primary), "The fixture key's first packet did not read.");
         size_t cut = primary.next;
         // Keep the user id and its certification, drop the subkey and its binding.
         Packet userId, certification;
         Require(ReadPacket(&aliceBytes[0], aliceBytes.size(), cut, userId) && ReadPacket(&aliceBytes[0], aliceBytes.size(), userId.next, certification), "The fixture key's user id did not read.");
         std::vector<unsigned char> primaryOnly(aliceBytes.begin(), aliceBytes.begin() + certification.next);
         const OpenPgpWriter::EncryptionKey none = OpenPgpWriter::ChooseEncryptionKey(&primaryOnly[0], primaryOnly.size(), 1800000000);
         Require(!none.ok && none.error.Find("no subkey that may encrypt") >= 0, "A key with no encryption subkey was not refused by name.");
         // Encrypting to a refused key is refused with its sentence.
         std::vector<unsigned char> nothing;
         Require(!OpenPgpWriter::Encrypt(none, (const unsigned char *) text, strlen(text), nothing, error) && error == none.error, "Encrypting to a refused key did not carry its sentence.");
      }

      // ---- an expiry, on the subkey or on the primary, is what the chooser answers
      {
         // Neither fixture key expires, and a signature can be given a
         // key-expiry subpacket without being re-made - the chooser, told not
         // to verify bindings, reads it as it stands - by putting
         // six octets at the front of the hashed area (RFC 9580 section
         // 5.2.3.13: a length of 5, the type 9, four octets of seconds after
         // the key's creation), the hashed length and the packet length each
         // stepped by six. WithExpiry does that to the signature that follows
         // the subkey (the binding) or the first user id (the self-certification).
         const __int64 primaryCreated = CreationTimeOf(aliceBytes, 6);
         const __int64 subkeyCreated = CreationTimeOf(aliceBytes, 14);
         Require(primaryCreated > 0 && subkeyCreated > 0, "The fixture key's creation times did not read.");

         const std::vector<unsigned char> subkeyExpiring = WithExpiry(aliceBytes, 14, 86400 * 365);
         Require(!subkeyExpiring.empty(), "The subkey binding signature could not be given an expiry.");

         // The binding was changed after alice signed it: with the binding
         // verified, the subkey is not hers, and nothing is left to encrypt to.
         const OpenPgpWriter::EncryptionKey forged = OpenPgpWriter::ChooseEncryptionKey(&subkeyExpiring[0], subkeyExpiring.size(), subkeyCreated + 1);
         Require(!forged.ok && forged.error.Find("not bound") >= 0, "A subkey whose binding was changed after it was signed was chosen.");

         const OpenPgpWriter::EncryptionKey living = OpenPgpWriter::ChooseEncryptionKey(&subkeyExpiring[0], subkeyExpiring.size(), subkeyCreated + 1, false);
         Require(living.ok, (AnsiString("A subkey that expires in a year was refused today: ") + living.error).c_str());
         Require(living.expires == subkeyCreated + 86400 * 365, "The chosen key's expiry is not the subkey binding's.");
         Require(!OpenPgpWriter::ChooseEncryptionKey(&subkeyExpiring[0], subkeyExpiring.size(), subkeyCreated + 86400 * 365 + 1, false).ok, "An expired subkey was chosen.");
         Require(OpenPgpWriter::ChooseEncryptionKey(&aliceBytes[0], aliceBytes.size(), subkeyCreated + 1).expires == 0, "A key with no expiry anywhere reports one.");

         // The primary's expiry, earlier than the subkey's, is the one answered:
         // the chooser refuses the whole key once the primary has expired.
         std::vector<unsigned char> bothExpiring = WithExpiry(subkeyExpiring, 13, 86400 * 30);
         Require(!bothExpiring.empty(), "The self-certification could not be given an expiry.");
         const OpenPgpWriter::EncryptionKey earlier = OpenPgpWriter::ChooseEncryptionKey(&bothExpiring[0], bothExpiring.size(), primaryCreated + 1, false);
         Require(earlier.ok, (AnsiString("A key whose primary expires in a month was refused today: ") + earlier.error).c_str());
         Require(earlier.expires == primaryCreated + 86400 * 30, "The earlier of the two expiries was not the one answered.");
         const OpenPgpWriter::EncryptionKey gone = OpenPgpWriter::ChooseEncryptionKey(&bothExpiring[0], bothExpiring.size(), primaryCreated + 86400 * 30 + 1, false);
         Require(!gone.ok && gone.error.Find("expired") >= 0, "A key whose primary has expired was not refused by name.");
      }

      // ---- a subkey appended under the primary, with a binding that is not its own
      {
         size_t offset = 0;
         Packet packet;
         Packet subkey;
         Packet binding;
         bool haveSubkey = false;
         bool haveBinding = false;
         while (ReadPacket(&aliceBytes[0], aliceBytes.size(), offset, packet))
         {
            if (packet.tag == TagPublicSubkey && !haveSubkey)
            {
               subkey = packet;
               haveSubkey = true;
            }
            else if (packet.tag == TagSignature && haveSubkey && !haveBinding)
            {
               binding = packet;
               haveBinding = true;
            }
            offset = packet.next;
         }
         Require(haveSubkey && haveBinding, "The fixture key's subkey and binding did not read.");

         std::vector<unsigned char> rogueBody(aliceBytes.begin() + subkey.body_start, aliceBytes.begin() + subkey.body_start + subkey.body_length);
         const unsigned int later = Read32(&rogueBody[1]) + 86400;
         rogueBody[1] = (unsigned char) ((later >> 24) & 0xFF);
         rogueBody[2] = (unsigned char) ((later >> 16) & 0xFF);
         rogueBody[3] = (unsigned char) ((later >> 8) & 0xFF);
         rogueBody[4] = (unsigned char) (later & 0xFF);

         std::vector<unsigned char> withRogue = aliceBytes;
         OpenPgpWriter::WritePacketHeader(TagPublicSubkey, rogueBody.size(), withRogue);
         withRogue.insert(withRogue.end(), rogueBody.begin(), rogueBody.end());
         // alice's own binding, header and all: it follows her subkey directly.
         Require(binding.body_start > subkey.next, "The fixture key's binding does not follow its subkey.");
         withRogue.insert(withRogue.end(), aliceBytes.begin() + subkey.next, aliceBytes.begin() + binding.next);

         const OpenPgpWriter::EncryptionKey kept = OpenPgpWriter::ChooseEncryptionKey(&withRogue[0], withRogue.size(), 1800000000);
         Require(kept.ok && kept.fingerprint == "4a144aada87aad8cce8a0659eaa03821629d0473",
                 "A newer subkey appended under the primary, with a binding that is not its own, was chosen over the one alice bound.");
         const OpenPgpWriter::EncryptionKey fooled = OpenPgpWriter::ChooseEncryptionKey(&withRogue[0], withRogue.size(), 1800000000, false);
         Require(fooled.ok && fooled.fingerprint != "4a144aada87aad8cce8a0659eaa03821629d0473",
                 "The rogue subkey was not the newer one, so the test above proved nothing.");
      }

      // ---- a subkey revocation: the key holder's counts, somebody else's does not
      {
         const AnsiString decoded = Base64::Decode(RevokedSubkeyBase64, (int) strlen(RevokedSubkeyBase64));
         const std::vector<unsigned char> revoked(decoded.c_str(), decoded.c_str() + decoded.GetLength());
         Require(!revoked.empty(), "The revoked-subkey fixture did not decode.");

         // GnuPG revoked the only encryption subkey, with a revocation its
         // primary made: nothing is left to encrypt to.
         const OpenPgpWriter::EncryptionKey refused = OpenPgpWriter::ChooseEncryptionKey(&revoked[0], revoked.size(), 1800000000);
         Require(!refused.ok && refused.error.Find("no subkey that may encrypt") >= 0, "A subkey its own primary revoked was chosen.");

         // That revocation, appended after alice's binding, was not made by
         // alice's primary over alice's subkey, so it revokes nothing of hers.
         size_t offset = 0;
         Packet packet;
         std::vector<unsigned char> revocation;
         while (ReadPacket(&revoked[0], revoked.size(), offset, packet))
         {
            if (packet.tag == TagSignature && packet.body_length > 2 && revoked[packet.body_start + 1] == 0x28)
            {
               revocation.assign(revoked.begin() + offset, revoked.begin() + packet.next);
               break;
            }
            offset = packet.next;
         }
         Require(!revocation.empty(), "The revoked-subkey fixture carries no subkey revocation.");

         std::vector<unsigned char> aliceRevoked = aliceBytes;
         aliceRevoked.insert(aliceRevoked.end(), revocation.begin(), revocation.end());
         const OpenPgpWriter::EncryptionKey still = OpenPgpWriter::ChooseEncryptionKey(&aliceRevoked[0], aliceRevoked.size(), 1800000000);
         Require(still.ok && still.fingerprint == "4a144aada87aad8cce8a0659eaa03821629d0473",
                 "A revocation somebody else's primary made revoked alice's subkey.");
         Require(!OpenPgpWriter::ChooseEncryptionKey(&aliceRevoked[0], aliceRevoked.size(), 1800000000, false).ok,
                 "The appended revocation is not read as one at all, so the test above proved nothing.");
      }
   }
}
