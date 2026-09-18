// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "WebAuthn.h"
#include "JsonDocument.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/sha.h>

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
      // A cursor over a CBOR document. Definite lengths only: an authenticator
      // sends the canonical encoding, and a document this reader cannot read
      // exactly is one it must refuse rather than half-read.
      class CborReader
      {
      public:
         CborReader(const std::string &bytes) : bytes_(bytes), at_(0) {}

         bool AtEnd() const { return at_ >= bytes_.size(); }
         size_t Position() const { return at_; }

         // The major type and the argument of the next item, stepping over both.
         bool Head(int &major, unsigned __int64 &argument)
         {
            if (at_ >= bytes_.size())
               return false;

            const unsigned char first = (unsigned char) bytes_[at_++];
            major = first >> 5;
            const unsigned char minor = first & 0x1f;

            if (minor < 24)
            {
               argument = minor;
               return true;
            }

            // 24, 25, 26, 27: one, two, four or eight octets of argument. 28 to
            // 30 are reserved and 31 is the indefinite length this refuses.
            size_t width = 0;
            switch (minor)
            {
            case 24: width = 1; break;
            case 25: width = 2; break;
            case 26: width = 4; break;
            case 27: width = 8; break;
            default: return false;
            }

            if (at_ + width > bytes_.size())
               return false;

            argument = 0;
            for (size_t i = 0; i < width; i++)
               argument = (argument << 8) | (unsigned char) bytes_[at_ + i];
            at_ += width;
            return true;
         }

         bool Bytes(size_t length, std::string &out)
         {
            // The subtraction, not the addition: at_ is never past the end, so
            // bytes_.size() - at_ is what is left, while at_ + length wraps for
            // a declared length near the top of size_t - and a wrapped sum is
            // smaller than the buffer, so the guard would pass, assign would
            // clamp to whatever happened to follow, and the cursor would go
            // BACKWARDS. A document declaring a length this reader cannot
            // honour is one it must refuse, not half-read.
            if (length > bytes_.size() - at_)
               return false;
            out.assign(bytes_, at_, length);
            at_ += length;
            return true;
         }

         // Reads one whole item and throws it away. Used for the attestation
         // statement, which this server does not verify.
         bool Skip(int depth = 0)
         {
            if (depth > 16)
               return false;

            int major = 0;
            unsigned __int64 argument = 0;
            if (!Head(major, argument))
               return false;

            switch (major)
            {
            case 0:   // unsigned
            case 1:   // negative
               return true;
            case 2:   // byte string
            case 3:   // text string
            {
               std::string ignored;
               return Bytes((size_t) argument, ignored);
            }
            case 4:   // array
               for (unsigned __int64 i = 0; i < argument; i++)
               {
                  if (!Skip(depth + 1))
                     return false;
               }
               return true;
            case 5:   // map
               for (unsigned __int64 i = 0; i < argument; i++)
               {
                  if (!Skip(depth + 1) || !Skip(depth + 1))
                     return false;
               }
               return true;
            case 7:   // simple value: false, true, null, undefined
               return argument <= 23;
            default:  // 6 is a tag, which nothing here sends
               return false;
            }
         }

      private:
         const std::string &bytes_;
         size_t at_;
      };

      unsigned int ReadBigEndian32(const std::string &bytes, size_t at)
      {
         return ((unsigned int) (unsigned char) bytes[at] << 24) |
                ((unsigned int) (unsigned char) bytes[at + 1] << 16) |
                ((unsigned int) (unsigned char) bytes[at + 2] << 8) |
                 (unsigned int) (unsigned char) bytes[at + 3];
      }

      // JWS carries an ECDSA signature as the raw R||S pair; WebAuthn carries
      // the X9.62 DER OpenSSL verifies, so nothing is transcoded here - the
      // signature is passed to OpenSSL as it arrived.
      EVP_PKEY *MakeEcKey(const std::string &x, const std::string &y)
      {
         if (x.size() != 32 || y.size() != 32)
            return nullptr;

         unsigned char point[65];
         point[0] = 0x04;
         memcpy(point + 1, x.data(), 32);
         memcpy(point + 33, y.data(), 32);

         EVP_PKEY *key = nullptr;
         OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
         OSSL_PARAM *params = nullptr;
         EVP_PKEY_CTX *ctx = nullptr;

         if (builder != nullptr &&
             OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, "P-256", 0) == 1 &&
             OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)) == 1)
         {
            params = OSSL_PARAM_BLD_to_param(builder);
            ctx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
            if (params != nullptr && ctx != nullptr &&
                EVP_PKEY_fromdata_init(ctx) == 1 &&
                EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
            {
               key = nullptr;
            }
         }

         if (ctx != nullptr) EVP_PKEY_CTX_free(ctx);
         if (params != nullptr) OSSL_PARAM_free(params);
         if (builder != nullptr) OSSL_PARAM_BLD_free(builder);
         return key;
      }

      EVP_PKEY *MakeRsaKey(const std::string &modulus, const std::string &exponent)
      {
         if (modulus.empty() || exponent.empty())
            return nullptr;

         BIGNUM *n = BN_bin2bn((const unsigned char *) modulus.data(), (int) modulus.size(), nullptr);
         BIGNUM *e = BN_bin2bn((const unsigned char *) exponent.data(), (int) exponent.size(), nullptr);

         EVP_PKEY *key = nullptr;
         OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
         OSSL_PARAM *params = nullptr;
         EVP_PKEY_CTX *ctx = nullptr;

         if (n != nullptr && e != nullptr && builder != nullptr &&
             OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) == 1 &&
             OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) == 1)
         {
            params = OSSL_PARAM_BLD_to_param(builder);
            ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
            if (params != nullptr && ctx != nullptr &&
                EVP_PKEY_fromdata_init(ctx) == 1 &&
                EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
            {
               key = nullptr;
            }
         }

         if (ctx != nullptr) EVP_PKEY_CTX_free(ctx);
         if (params != nullptr) OSSL_PARAM_free(params);
         if (builder != nullptr) OSSL_PARAM_BLD_free(builder);
         if (n != nullptr) BN_free(n);
         if (e != nullptr) BN_free(e);

         // Under 2048 bits is a key no authenticator makes and one this server
         // should not accept a signature from.
         if (key != nullptr && EVP_PKEY_get_bits(key) < 2048)
         {
            EVP_PKEY_free(key);
            key = nullptr;
         }

         return key;
      }
   }

   AnsiString
   WebAuthn::Base64Url(const unsigned char *data, size_t length)
   {
      static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
      std::string out;
      out.reserve((length * 4) / 3 + 3);

      size_t i = 0;
      for (; i + 2 < length; i += 3)
      {
         const unsigned int n = ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8) | data[i + 2];
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
         out += alphabet[n & 63];
      }

      if (i + 1 == length)
      {
         const unsigned int n = (unsigned int) data[i] << 16;
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
      }
      else if (i + 2 == length)
      {
         const unsigned int n = ((unsigned int) data[i] << 16) | ((unsigned int) data[i + 1] << 8);
         out += alphabet[(n >> 18) & 63];
         out += alphabet[(n >> 12) & 63];
         out += alphabet[(n >> 6) & 63];
      }

      return AnsiString(out.c_str());
   }

   AnsiString
   WebAuthn::Base64Url(const std::string &bytes)
   {
      return Base64Url((const unsigned char *) bytes.data(), bytes.size());
   }

   bool
   WebAuthn::Base64UrlDecode(const std::string &text, std::string &bytes)
   {
      bytes.clear();

      unsigned int accumulator = 0;
      int bits = 0;
      for (size_t i = 0; i < text.size(); i++)
      {
         const char c = text[i];
         int value;
         if (c >= 'A' && c <= 'Z') value = c - 'A';
         else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
         else if (c >= '0' && c <= '9') value = c - '0' + 52;
         else if (c == '-') value = 62;
         else if (c == '_') value = 63;
         else if (c == '=') continue;      // padding a client added anyway
         else return false;

         accumulator = (accumulator << 6) | (unsigned int) value;
         bits += 6;
         if (bits >= 8)
         {
            bits -= 8;
            bytes += (char) ((accumulator >> bits) & 0xff);
         }
      }

      return true;
   }

   std::string
   WebAuthn::Sha256(const std::string &bytes)
   {
      unsigned char digest[SHA256_DIGEST_LENGTH];
      SHA256((const unsigned char *) bytes.data(), bytes.size(), digest);
      return std::string((const char *) digest, sizeof(digest));
   }

   bool
   WebAuthn::ReadClientData(const std::string &json, ClientData &data, AnsiString &problem)
   {
      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(json, document, parseError) || !document.IsObject())
      {
         problem = "the client data is not a JSON object";
         return false;
      }

      data.type = document.GetString("type");
      data.challenge = document.GetString("challenge");
      data.origin = document.GetString("origin");
      data.cross_origin = document.GetBool("crossOrigin", false);

      if (data.type.empty() || data.challenge.empty() || data.origin.empty())
      {
         problem = "the client data names no type, challenge or origin";
         return false;
      }

      return true;
   }

   bool
   WebAuthn::ReadAuthenticatorData(const std::string &bytes, AuthenticatorData &data, AnsiString &problem)
   {
      // 32 octets of rp id hash, one of flags, four of counter.
      if (bytes.size() < 37)
      {
         problem = "the authenticator data is shorter than its fixed part";
         return false;
      }

      data.rp_id_hash = bytes.substr(0, 32);
      data.flags = (unsigned char) bytes[32];
      data.sign_count = ReadBigEndian32(bytes, 33);
      data.attested = (data.flags & FlagAttestedData) != 0;
      data.aaguid.clear();
      data.credential_id.clear();
      data.public_key.clear();

      if (!data.attested)
         return true;

      // Attested credential data: 16 octets of AAGUID, two of credential id
      // length, the id, then the COSE key - and possibly extensions after it,
      // which is why the key's own length is taken from where the CBOR ends.
      if (bytes.size() < 55)
      {
         problem = "the authenticator data claims attested credential data and is too short to hold it";
         return false;
      }

      data.aaguid = bytes.substr(37, 16);
      const size_t idLength = ((size_t) (unsigned char) bytes[53] << 8) | (unsigned char) bytes[54];
      if (idLength == 0 || idLength > 1023 || bytes.size() < 55 + idLength)
      {
         problem = "the credential id is missing or longer than the authenticator data";
         return false;
      }

      data.credential_id = bytes.substr(55, idLength);

      const std::string remainder = bytes.substr(55 + idLength);
      CborReader reader(remainder);
      if (!reader.Skip())
      {
         problem = "the credential public key is not readable CBOR";
         return false;
      }

      data.public_key = remainder.substr(0, reader.Position());
      return true;
   }

   bool
   WebAuthn::ReadAttestationObject(const std::string &bytes, std::string &authenticatorData,
                                   AnsiString &format, AnsiString &problem)
   {
      authenticatorData.clear();
      format = "";

      CborReader reader(bytes);
      int major = 0;
      unsigned __int64 count = 0;
      if (!reader.Head(major, count) || major != 5 || count == 0 || count > 8)
      {
         problem = "the attestation object is not a CBOR map";
         return false;
      }

      for (unsigned __int64 i = 0; i < count; i++)
      {
         int keyMajor = 0;
         unsigned __int64 keyLength = 0;
         if (!reader.Head(keyMajor, keyLength) || keyMajor != 3)
         {
            problem = "the attestation object has a key that is not text";
            return false;
         }

         std::string key;
         if (!reader.Bytes((size_t) keyLength, key))
         {
            problem = "the attestation object ends inside a key";
            return false;
         }

         if (key == "fmt")
         {
            int valueMajor = 0;
            unsigned __int64 valueLength = 0;
            std::string value;
            if (!reader.Head(valueMajor, valueLength) || valueMajor != 3 || !reader.Bytes((size_t) valueLength, value))
            {
               problem = "the attestation object's fmt is not text";
               return false;
            }
            format = AnsiString(value.c_str());
         }
         else if (key == "authData")
         {
            int valueMajor = 0;
            unsigned __int64 valueLength = 0;
            if (!reader.Head(valueMajor, valueLength) || valueMajor != 2 ||
                !reader.Bytes((size_t) valueLength, authenticatorData))
            {
               problem = "the attestation object's authData is not a byte string";
               return false;
            }
         }
         else
         {
            // attStmt and anything else. Stepped over: the statement is not
            // verified, and WebAuthn.h says why.
            if (!reader.Skip())
            {
               problem = "the attestation object holds a value this server cannot read";
               return false;
            }
         }
      }

      if (authenticatorData.empty())
      {
         problem = "the attestation object carries no authenticator data";
         return false;
      }

      return true;
   }

   bool
   WebAuthn::ReadPublicKey(const std::string &cose, PublicKey &key, AnsiString &problem)
   {
      key = PublicKey();

      CborReader reader(cose);
      int major = 0;
      unsigned __int64 count = 0;
      if (!reader.Head(major, count) || major != 5 || count == 0 || count > 16)
      {
         problem = "the credential public key is not a COSE map";
         return false;
      }

      int keyType = 0;
      std::string first, second, third;
      bool haveFirst = false, haveSecond = false, haveThird = false;
      int curve = 0;

      for (unsigned __int64 i = 0; i < count; i++)
      {
         int labelMajor = 0;
         unsigned __int64 labelValue = 0;
         if (!reader.Head(labelMajor, labelValue) || (labelMajor != 0 && labelMajor != 1))
         {
            problem = "the credential public key has a label that is not an integer";
            return false;
         }

         // Major 1 is a negative integer whose value is -1 - argument.
         const __int64 label = labelMajor == 0 ? (__int64) labelValue : -1 - (__int64) labelValue;

         int valueMajor = 0;
         unsigned __int64 valueArgument = 0;
         if (!reader.Head(valueMajor, valueArgument))
         {
            problem = "the credential public key ends inside a value";
            return false;
         }

         std::string bytes;
         if (valueMajor == 2)
         {
            if (!reader.Bytes((size_t) valueArgument, bytes))
            {
               problem = "the credential public key ends inside a byte string";
               return false;
            }
         }
         else if (valueMajor != 0 && valueMajor != 1)
         {
            // A COSE key carries integers and byte strings and nothing else.
            // Anything more is a document this reader cannot read exactly, and
            // half-reading a public key is how the wrong key gets stored.
            problem = "the credential public key holds a value this server cannot read";
            return false;
         }

         const __int64 number = valueMajor == 0 ? (__int64) valueArgument
                              : valueMajor == 1 ? -1 - (__int64) valueArgument : 0;

         switch (label)
         {
         case 1:  keyType = (int) number; break;
         case 3:  key.algorithm = (int) number; break;
         case -1: curve = (int) number; first = bytes; haveFirst = true; break;
         case -2: second = bytes; haveSecond = true; break;
         case -3: third = bytes; haveThird = true; break;
         default: break;
         }
      }

      // kty 2 is EC2 and 3 is RSA (RFC 8152 table 21). The algorithm has to
      // agree with the key type, or an EC key labelled RS256 would reach
      // OpenSSL and fail with "signature verification failed", which describes
      // the symptom of an algorithm confusion rather than the cause.
      if (keyType == 2 && key.algorithm == AlgorithmEs256)
      {
         if (curve != 1)
         {
            problem = "the credential is on a curve other than P-256";
            return false;
         }
         if (!haveSecond || !haveThird || second.size() != 32 || third.size() != 32)
         {
            problem = "the credential public key has no P-256 point";
            return false;
         }
         key.x = second;
         key.y = third;
         return true;
      }

      if (keyType == 3 && key.algorithm == AlgorithmRs256)
      {
         if (!haveFirst || !haveSecond || first.empty() || second.empty())
         {
            problem = "the credential public key has no RSA modulus and exponent";
            return false;
         }
         key.modulus = first;
         key.exponent = second;
         return true;
      }

      problem = "the credential's algorithm is not one this server can verify (ES256 or RS256)";
      return false;
   }

   bool
   WebAuthn::VerifySignature(const PublicKey &key, const std::string &authenticatorData,
                             const std::string &clientDataJson, const std::string &signature)
   {
      if (signature.empty())
         return false;

      EVP_PKEY *pkey = key.algorithm == AlgorithmEs256 ? MakeEcKey(key.x, key.y)
                     : key.algorithm == AlgorithmRs256 ? MakeRsaKey(key.modulus, key.exponent)
                     : nullptr;
      if (pkey == nullptr)
         return false;

      // What an authenticator signs: the authenticator data, then the hash of
      // the client data - never the client data itself.
      const std::string signed_ = authenticatorData + Sha256(clientDataJson);

      bool verified = false;
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (ctx != nullptr)
      {
         if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
             EVP_DigestVerifyUpdate(ctx, signed_.data(), signed_.size()) == 1)
         {
            verified = EVP_DigestVerifyFinal(ctx, (const unsigned char *) signature.data(), signature.size()) == 1;
         }
         EVP_MD_CTX_free(ctx);
      }

      EVP_PKEY_free(pkey);
      return verified;
   }

   // ------------------------------------------------------------------------
   // The self-test, against vectors a software authenticator produced.
   //
   // They were made by build-time throwaway script with the Python cryptography
   // library: a P-256 key from a fixed scalar and a 2048-bit RSA key, a
   // registration for the relying party mail.example.test at the origin
   // https://mail.example.test, and two assertions signed exactly as an
   // authenticator signs one - over authenticatorData || SHA-256(clientDataJSON).
   // They are constants here rather than generated, because a round trip
   // against this file's own encoder would prove only that it agrees with
   // itself.
   // ------------------------------------------------------------------------
   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      std::string FromHex(const char *text)
      {
         std::string bytes;
         for (const char *at = text; at[0] != 0 && at[1] != 0; at += 2)
         {
            int value = 0;
            for (int i = 0; i < 2; i++)
            {
               const char c = at[i];
               const int digit = c >= '0' && c <= '9' ? c - '0'
                               : c >= 'a' && c <= 'f' ? c - 'a' + 10
                               : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
               if (digit < 0)
                  throw std::logic_error("WebAuthnTester: a vector is not hexadecimal");
               value = (value << 4) | digit;
            }
            bytes += (char) value;
         }
         return bytes;
      }

      const char *AttestationObjectHex =
         "a363666d74646e6f6e656761747453746d74a068617574684461746158a46467aa1b3854a867205c74a1f0203af1940e"
         "d898d8e15a78608191add5c040344500000001000102030405060708090a0b0c0d0e0f0020a0a1a2a3a4a5a6a7a8a9aa"
         "abacadaeafa0a1a2a3a4a5a6a7a8a9aaabacadaeafa5010203262001215820842d0de78b94c5735fd82f7b683e62d6c4"
         "5e75d7d1d6993b121ca5efe0a36846225820aa0819f381028e9e1080b57efcfdf1f4ce2dd614a73f222e383ddfb2e417"
         "bdb2";
      const char *CreateClientDataHex =
         "7b2274797065223a22776562617574686e2e637265617465222c226368616c6c656e6765223a22455245524552455245"
         "52455245524552455245524552455245524552455245524552455245524552455245222c226f726967696e223a226874"
         "7470733a2f2f6d61696c2e6578616d706c652e74657374222c2263726f73734f726967696e223a66616c73657d";
      const char *CoseEcHex =
         "a5010203262001215820842d0de78b94c5735fd82f7b683e62d6c45e75d7d1d6993b121ca5efe0a36846225820aa0819"
         "f381028e9e1080b57efcfdf1f4ce2dd614a73f222e383ddfb2e417bdb2";
      const char *AssertionAuthDataHex =
         "6467aa1b3854a867205c74a1f0203af1940ed898d8e15a78608191add5c040340500000007";
      const char *GetClientDataHex =
         "7b2274797065223a22776562617574686e2e676574222c226368616c6c656e6765223a22496949694969496949694969"
         "49694969496949694969496949694969496949694969496949694969496949222c226f726967696e223a226874747073"
         "3a2f2f6d61696c2e6578616d706c652e74657374222c2263726f73734f726967696e223a66616c73657d";
      const char *EcSignatureHex =
         "3045022028f820a805bd24ac32faf90f20b3227a3ced64eb86c84980158710c3658c4001022100954ae7ecf76e8f5778"
         "424d8c5502a2cbf843a11f85249601b91a84047c174fe2";
      const char *CoseRsaHex =
         "a401030339010020590100e4ae6030b7f8520f29727de4ff7baee1eebdacebd3606a2499c92cb6eae818fa7490e8895d"
         "fc6fc3595d668fcc26ba78bd466dee318ccd993215ccf85cfec0eb957cbe8006353c777c93e3970bfdd0242adccb9967"
         "a6018b862a92edf41858b39019215eeaea718bb0a4cd2a66072ecd33b94224599bae53578e07bf2d13cff281624aa5fa"
         "6c74d67338de916563db1cbf8b5d981b2d78c74c117e377d1eb1fcf1bb44e5729e8a3cf19f4e7f7718ff892e8f8c7cc2"
         "e5d1be889219e39a737a83dff81519f4dda57fe46b10b06471dbe9f6d5d0b7ab5f53885a8e4afce189646dcf959a4505"
         "f299ec0b3a6199cddc2d809bc874eb1d257281905f7114f59b38852143010001";
      const char *RsaAuthDataHex =
         "6467aa1b3854a867205c74a1f0203af1940ed898d8e15a78608191add5c040340500000003";
      const char *RsaClientDataHex =
         "7b2274797065223a22776562617574686e2e676574222c226368616c6c656e6765223a224d7a4d7a4d7a4d7a4d7a4d7a"
         "4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d7a4d222c226f726967696e223a226874747073"
         "3a2f2f6d61696c2e6578616d706c652e74657374222c2263726f73734f726967696e223a66616c73657d";
      const char *RsaSignatureHex =
         "71f4525be3654076ebd690faec1748cc4e0a4f6b3b82d94050b67026b91464d1f466c31bc3f0587e320689caf51d0a80"
         "a9acfc937a7c7d461ff822926551d2b4ed1cf75d78b0961eeac65a163dc134788fbf6e4f8179b79e29231a9185641bbd"
         "20fc230258e381e075a3d87b59bee51b16e466811a259271a99f2fe573ee6b560c8fa65b5c79c10c64758a6af1ad1d96"
         "566e66ff62d777e37e665fcb72854ca64ba3ae684d9c8c5e4b44faf7bbba809157ff732a570ce85325564ecd554daa7b"
         "47f4b2fcd4caf88d43f7633f945d496d7fe8c6cbf5ac667ea8c63d0cdd7a6c3f3dcb7475f21841cbefc74680a75f4369"
         "23a8480cd8eb9bdfa83ad9439c721e0c";
   }

   void
   WebAuthnTester::Test()
   {
      typedef WebAuthn W;

      // base64url, both ways, against RFC 4648's own vectors.
      const unsigned char f[] = { 'f' };
      const unsigned char fo[] = { 'f', 'o' };
      const unsigned char foo[] = { 'f', 'o', 'o' };
      const unsigned char high[] = { 0xfb, 0xff };
      Expect(W::Base64Url(f, 1) == "Zg" && W::Base64Url(fo, 2) == "Zm8" && W::Base64Url(foo, 3) == "Zm9v",
             "base64url of RFC 4648's vectors, unpadded.");
      Expect(W::Base64Url(high, 2) == "-_8", "base64url writes - and _ where base64 writes + and /.");

      std::string decoded;
      Expect(W::Base64UrlDecode("Zm9v", decoded) && decoded == "foo", "base64url decodes.");
      Expect(!W::Base64UrlDecode("Zm9v*", decoded), "A character outside the alphabet is refused.");

      const std::string rpIdHash = W::Sha256("mail.example.test");

      // ---- A registration -------------------------------------------------
      const std::string attestation = FromHex(AttestationObjectHex);
      std::string authenticatorData;
      AnsiString format, problem;
      Expect(W::ReadAttestationObject(attestation, authenticatorData, format, problem),
             "The attestation object reads.");
      Expect(format == "none", "Its format is none, which is what the options ask for.");

      W::AuthenticatorData registration;
      Expect(W::ReadAuthenticatorData(authenticatorData, registration, problem),
             "The registration's authenticator data reads.");
      Expect(registration.rp_id_hash == rpIdHash, "The rp id hash is SHA-256 of the relying party id.");
      Expect((registration.flags & W::FlagUserPresent) != 0 && (registration.flags & W::FlagUserVerified) != 0,
             "The person was present and was verified.");
      Expect(registration.attested && registration.aaguid.size() == 16, "There is attested credential data.");
      Expect(registration.sign_count == 1, "The counter starts where the authenticator says.");
      Expect(W::Base64Url(registration.credential_id) == "oKGio6SlpqeoqaqrrK2ur6ChoqOkpaanqKmqq6ytrq8",
             "The credential id is read whole.");
      Expect(registration.public_key == FromHex(CoseEcHex),
             "The COSE key is taken by its exact bytes, so it can be stored and used again.");

      W::ClientData created;
      Expect(W::ReadClientData(FromHex(CreateClientDataHex), created, problem), "The client data reads.");
      Expect(created.type == "webauthn.create", "A registration says webauthn.create.");
      Expect(created.origin == "https://mail.example.test", "It names the origin it was collected at.");
      Expect(created.challenge == "ERERERERERERERERERERERERERERERERERERERERERE",
             "And echoes the challenge the server sent.");

      // ---- An assertion, ES256 --------------------------------------------
      W::PublicKey key;
      Expect(W::ReadPublicKey(registration.public_key, key, problem), "The COSE key reads.");
      Expect(key.algorithm == W::AlgorithmEs256 && key.x.size() == 32 && key.y.size() == 32,
             "It is an ES256 key on P-256.");

      const std::string assertionAuth = FromHex(AssertionAuthDataHex);
      const std::string getClient = FromHex(GetClientDataHex);
      const std::string signature = FromHex(EcSignatureHex);

      W::AuthenticatorData assertion;
      Expect(W::ReadAuthenticatorData(assertionAuth, assertion, problem), "The assertion's authenticator data reads.");
      Expect(!assertion.attested && assertion.sign_count == 7,
             "An assertion carries no attested credential data, and the counter has moved on.");
      Expect(assertion.rp_id_hash == rpIdHash, "It is for the same relying party.");

      Expect(W::VerifySignature(key, assertionAuth, getClient, signature), "The ES256 signature verifies.");

      // The three things a forgery would change. Each must fail on its own.
      std::string tampered = assertionAuth;
      tampered[36] = (char) (tampered[36] + 1);
      Expect(!W::VerifySignature(key, tampered, getClient, signature),
             "A changed counter is not the data that was signed.");

      std::string otherClient = getClient;
      const size_t originAt = otherClient.find("mail.example.test");
      Expect(originAt != std::string::npos, "The vector's client data names the origin.");
      otherClient.replace(originAt, 4, "evil");
      Expect(!W::VerifySignature(key, assertionAuth, otherClient, signature),
             "Client data collected at another origin is not what was signed.");

      std::string bentSignature = signature;
      bentSignature[bentSignature.size() - 1] = (char) (bentSignature[bentSignature.size() - 1] ^ 0x01);
      Expect(!W::VerifySignature(key, assertionAuth, getClient, bentSignature), "A bent signature is refused.");

      // ---- An assertion, RS256 --------------------------------------------
      W::PublicKey rsa;
      Expect(W::ReadPublicKey(FromHex(CoseRsaHex), rsa, problem), "An RSA COSE key reads.");
      Expect(rsa.algorithm == W::AlgorithmRs256 && rsa.modulus.size() == 256 && rsa.exponent.size() == 3,
             "It is an RS256 key with a 2048-bit modulus.");
      Expect(W::VerifySignature(rsa, FromHex(RsaAuthDataHex), FromHex(RsaClientDataHex), FromHex(RsaSignatureHex)),
             "The RS256 signature verifies.");
      Expect(!W::VerifySignature(rsa, FromHex(RsaAuthDataHex), FromHex(GetClientDataHex), FromHex(RsaSignatureHex)),
             "With other client data it does not.");

      // ---- What the readers refuse ----------------------------------------
      W::AuthenticatorData tooShort;
      Expect(!W::ReadAuthenticatorData(std::string(36, '\0'), tooShort, problem),
             "Authenticator data shorter than its fixed part is refused.");

      std::string ignored;
      AnsiString ignoredFormat;
      Expect(!W::ReadAttestationObject("\x5f", ignored, ignoredFormat, problem),
             "An indefinite-length map is refused rather than guessed at.");
      Expect(!W::ReadAttestationObject("", ignored, ignoredFormat, problem), "So is nothing at all.");

      // A byte string that declares the largest length there is, with real
      // authenticator data behind it. The declared span is not there, so the
      // document is refused: an accepted one would be a half-read attestation
      // object handing on whatever bytes happened to follow, which is the
      // thing WebAuthn.h says this reader never does.
      Expect(!W::ReadAttestationObject(FromHex("a16861757468446174615bffffffffffffffff"
                                               "1111111111111111111111111111111111111111111111111111111111111111"
                                               "4500000001"),
                                       ignored, ignoredFormat, problem),
             "A byte string longer than the document is refused, not clamped to what follows it.");

      W::PublicKey unusable;
      // kty EC2 with alg -257: the algorithm confusion this refuses by name.
      Expect(!W::ReadPublicKey(FromHex("a40102033901002001215820842d0de78b94c5735fd82f7b683e62d6c45e75d7d1d6993b121ca5efe0a36846"),
                               unusable, problem),
             "A key whose type and algorithm disagree is refused.");

      W::ClientData notJson;
      Expect(!W::ReadClientData("not json", notJson, problem), "Client data that is not JSON is refused.");
   }
}
