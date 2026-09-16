// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "WebPush.h"

#include "Encoding/Base64.h"

#include <stdexcept>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // P-256, and every length that follows from it. The uncompressed point is a
      // 0x04 lead octet and two 32-octet coordinates; the record size is the 4096
      // of RFC 8291's example, which every push service accepts and which bounds
      // the plaintext at rs - 16 (the GCM tag) - 1 (the padding delimiter).
      const char *GroupName = "prime256v1";
      const size_t CoordinateLength = 32;
      const size_t PointLength = 65;
      const size_t SaltLength = 16;
      const size_t KeyLength = 16;      // AES-128
      const size_t NonceLength = 12;
      const size_t TagLength = 16;
      const unsigned int RecordSize = 4096;

      // RFC 8291 3.4 and RFC 8188 2.2. Written out rather than built, because a
      // typo in one of them is a push nobody can read and no error anywhere.
      const char *KeyInfoPrefix = "WebPush: info";
      const char *CekInfo = "Content-Encoding: aes128gcm";
      const char *NonceInfo = "Content-Encoding: nonce";

      typedef std::vector<unsigned char> Bytes;

      void Append(Bytes &target, const char *text)
      {
         target.insert(target.end(), text, text + strlen(text));
      }

      void Append(Bytes &target, const Bytes &source)
      {
         target.insert(target.end(), source.begin(), source.end());
      }

      // HMAC-SHA256, which is every step of the derivation: HKDF-Extract is one
      // HMAC and each HKDF-Expand here is one block, since nothing derived is
      // longer than 32 octets.
      bool Hmac(const Bytes &key, const Bytes &data, Bytes &output)
      {
         output.assign(32, 0);
         unsigned int length = 0;

         // An empty key is a real key to HMAC (it pads to the block size), but
         // OpenSSL wants a non-null pointer for it.
         const unsigned char *keyData = key.empty() ? (const unsigned char*) "" : key.data();

         if (HMAC(EVP_sha256(), keyData, (int) key.size(), data.data(), data.size(), output.data(), &length) == nullptr ||
             length != 32)
         {
            output.clear();
            return false;
         }

         return true;
      }

      // HKDF-Expand for one block, truncated to length. RFC 5869: the info with a
      // 0x01 counter appended, under the pseudorandom key.
      bool Expand(const Bytes &prk, const Bytes &info, size_t length, Bytes &output)
      {
         Bytes data = info;
         data.push_back(0x01);

         Bytes block;
         if (!Hmac(prk, data, block) || length > block.size())
            return false;

         output.assign(block.begin(), block.begin() + length);
         return true;
      }

      // A public key from its uncompressed point.
      EVP_PKEY *PublicKeyFromPoint(const Bytes &point)
      {
         if (point.size() != PointLength || point[0] != 0x04)
            return nullptr;

         EVP_PKEY *key = nullptr;
         OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();

         if (builder != nullptr &&
             OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, GroupName, 0) == 1 &&
             OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) == 1)
         {
            OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
            if (params != nullptr)
            {
               EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
               if (context != nullptr)
               {
                  if (EVP_PKEY_fromdata_init(context) != 1 ||
                      EVP_PKEY_fromdata(context, &key, EVP_PKEY_PUBLIC_KEY, params) != 1)
                     key = nullptr;

                  EVP_PKEY_CTX_free(context);
               }

               OSSL_PARAM_free(params);
            }
         }

         if (builder != nullptr)
            OSSL_PARAM_BLD_free(builder);

         return key;
      }

      // The public point of a private scalar: d * G, worked out with the group
      // rather than assumed, so a stored private key alone is enough to rebuild a
      // whole key pair.
      bool PointOfScalar(const Bytes &scalar, Bytes &point)
      {
         point.clear();

         if (scalar.size() != CoordinateLength)
            return false;

         EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
         if (group == nullptr)
            return false;

         BIGNUM *value = BN_bin2bn(scalar.data(), (int) scalar.size(), nullptr);
         EC_POINT *result = value != nullptr ? EC_POINT_new(group) : nullptr;
         bool ok = false;

         if (result != nullptr && EC_POINT_mul(group, result, value, nullptr, nullptr, nullptr) == 1)
         {
            point.assign(PointLength, 0);
            ok = EC_POINT_point2oct(group, result, POINT_CONVERSION_UNCOMPRESSED,
                                    point.data(), point.size(), nullptr) == PointLength;
         }

         if (result != nullptr)
            EC_POINT_free(result);
         if (value != nullptr)
            BN_clear_free(value);
         EC_GROUP_free(group);

         if (!ok)
            point.clear();

         return ok;
      }

      // A key pair from the private scalar. The public half has to be supplied
      // beside it - OpenSSL's provider will not derive it for us here - so it is
      // computed from the scalar first.
      EVP_PKEY *PrivateKeyFromScalar(const Bytes &scalar)
      {
         Bytes point;
         if (!PointOfScalar(scalar, point))
            return nullptr;

         EVP_PKEY *key = nullptr;
         BIGNUM *value = BN_bin2bn(scalar.data(), (int) scalar.size(), nullptr);
         OSSL_PARAM_BLD *builder = value != nullptr ? OSSL_PARAM_BLD_new() : nullptr;

         if (builder != nullptr &&
             OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, GroupName, 0) == 1 &&
             OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) == 1 &&
             OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_PRIV_KEY, value) == 1)
         {
            OSSL_PARAM *params = OSSL_PARAM_BLD_to_param(builder);
            if (params != nullptr)
            {
               EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
               if (context != nullptr)
               {
                  if (EVP_PKEY_fromdata_init(context) != 1 ||
                      EVP_PKEY_fromdata(context, &key, EVP_PKEY_KEYPAIR, params) != 1)
                     key = nullptr;

                  EVP_PKEY_CTX_free(context);
               }

               OSSL_PARAM_free(params);
            }
         }

         if (builder != nullptr)
            OSSL_PARAM_BLD_free(builder);
         if (value != nullptr)
            BN_clear_free(value);

         return key;
      }

      // The ECDH shared secret: the x coordinate of the product, 32 octets, which
      // is what EVP_PKEY_derive gives for a named prime curve.
      bool SharedSecret(EVP_PKEY *own, EVP_PKEY *peer, Bytes &secret)
      {
         secret.clear();

         EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(own, nullptr);
         if (context == nullptr)
            return false;

         size_t length = 0;
         bool ok = EVP_PKEY_derive_init(context) == 1 &&
                   EVP_PKEY_derive_set_peer(context, peer) == 1 &&
                   EVP_PKEY_derive(context, nullptr, &length) == 1 &&
                   length == CoordinateLength;

         if (ok)
         {
            secret.assign(length, 0);
            ok = EVP_PKEY_derive(context, secret.data(), &length) == 1 && length == CoordinateLength;
         }

         EVP_PKEY_CTX_free(context);

         if (!ok)
            secret.clear();

         return ok;
      }

      // RFC 8291 3.4 and RFC 8188 2.2, in one place, because the two halves are
      // never right apart: the auth secret combines the ECDH secret into the
      // input keying material, and the salt then extracts the key and the nonce.
      //
      // uaPublic is always the RECEIVER's public key and asPublic always the
      // SENDER's, whichever direction this is running in - key_info is not
      // symmetric, and swapping them is the mistake that makes a body encrypt and
      // decrypt happily here and be unreadable in a browser.
      bool DeriveKeyAndNonce(const Bytes &ecdhSecret, const Bytes &authSecret, const Bytes &uaPublic,
                             const Bytes &asPublic, const Bytes &salt, Bytes &key, Bytes &nonce)
      {
         Bytes prkKey;
         if (!Hmac(authSecret, ecdhSecret, prkKey))
            return false;

         Bytes keyInfo;
         Append(keyInfo, KeyInfoPrefix);
         keyInfo.push_back(0x00);
         Append(keyInfo, uaPublic);
         Append(keyInfo, asPublic);

         Bytes ikm;
         if (!Expand(prkKey, keyInfo, 32, ikm))
            return false;

         Bytes prk;
         if (!Hmac(salt, ikm, prk))
            return false;

         Bytes cekInfo;
         Append(cekInfo, CekInfo);
         cekInfo.push_back(0x00);

         Bytes nonceInfo;
         Append(nonceInfo, NonceInfo);
         nonceInfo.push_back(0x00);

         return Expand(prk, cekInfo, KeyLength, key) && Expand(prk, nonceInfo, NonceLength, nonce);
      }

      bool AesGcmSeal(const Bytes &key, const Bytes &nonce, const Bytes &plainText, Bytes &cipherText)
      {
         cipherText.assign(plainText.size() + TagLength, 0);

         EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
         if (context == nullptr)
            return false;

         int length = 0;
         bool ok = EVP_EncryptInit_ex(context, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
                   EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int) nonce.size(), nullptr) == 1 &&
                   EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;

         if (ok && !plainText.empty())
            ok = EVP_EncryptUpdate(context, cipherText.data(), &length, plainText.data(), (int) plainText.size()) == 1 &&
                 (size_t) length == plainText.size();

         ok = ok && EVP_EncryptFinal_ex(context, cipherText.data() + plainText.size(), &length) == 1 && length == 0;
         ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, (int) TagLength,
                                        cipherText.data() + plainText.size()) == 1;

         EVP_CIPHER_CTX_free(context);

         if (!ok)
            cipherText.clear();

         return ok;
      }

      bool AesGcmOpen(const Bytes &key, const Bytes &nonce, const Bytes &cipherText, Bytes &plainText)
      {
         plainText.clear();

         if (cipherText.size() < TagLength)
            return false;

         size_t bodyLength = cipherText.size() - TagLength;
         plainText.assign(bodyLength, 0);

         EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
         if (context == nullptr)
            return false;

         int length = 0;
         bool ok = EVP_DecryptInit_ex(context, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
                   EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int) nonce.size(), nullptr) == 1 &&
                   EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1;

         if (ok && bodyLength > 0)
            ok = EVP_DecryptUpdate(context, plainText.data(), &length, cipherText.data(), (int) bodyLength) == 1 &&
                 (size_t) length == bodyLength;

         ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, (int) TagLength,
                                        const_cast<unsigned char*>(cipherText.data() + bodyLength)) == 1;
         ok = ok && EVP_DecryptFinal_ex(context, plainText.data() + bodyLength, &length) == 1 && length == 0;

         EVP_CIPHER_CTX_free(context);

         if (!ok)
            plainText.clear();

         return ok;
      }

      // A JWS signature is r and s as fixed-width octets, one after the other -
      // not the DER sequence OpenSSL signs into. The conversion is by value and
      // left-padded: an r shorter than 32 octets that is written without its
      // leading zeros is a signature every verifier rejects.
      bool DerToRawSignature(const Bytes &der, Bytes &raw)
      {
         raw.clear();

         const unsigned char *pointer = der.data();
         ECDSA_SIG *signature = d2i_ECDSA_SIG(nullptr, &pointer, (long) der.size());
         if (signature == nullptr)
            return false;

         const BIGNUM *r = nullptr;
         const BIGNUM *s = nullptr;
         ECDSA_SIG_get0(signature, &r, &s);

         bool ok = r != nullptr && s != nullptr &&
                   BN_num_bytes(r) <= (int) CoordinateLength && BN_num_bytes(s) <= (int) CoordinateLength;

         if (ok)
         {
            raw.assign(CoordinateLength * 2, 0);
            BN_bn2binpad(r, raw.data(), (int) CoordinateLength);
            BN_bn2binpad(s, raw.data() + CoordinateLength, (int) CoordinateLength);
         }

         ECDSA_SIG_free(signature);
         return ok;
      }
   }

   AnsiString
   WebPush::Base64UrlEncode(const unsigned char *data, size_t length)
   {
      AnsiString encoded = Base64::Encode((const char*) data, (int) length);

      AnsiString result;
      for (int i = 0; i < encoded.GetLength(); i++)
      {
         char character = encoded[i];

         switch (character)
         {
         case '+':
            result += "-";
            break;
         case '/':
            result += "_";
            break;
         case '=':
         case '\r':
         case '\n':
            break;
         default:
            result += character;
            break;
         }
      }

      return result;
   }

   AnsiString
   WebPush::Base64UrlEncode(const std::vector<unsigned char> &data)
   {
      return Base64UrlEncode(data.empty() ? (const unsigned char*) "" : data.data(), data.size());
   }

   bool
   WebPush::Base64UrlDecode(const AnsiString &text, std::vector<unsigned char> &output)
   {
      output.clear();

      AnsiString standard;
      for (int i = 0; i < text.GetLength(); i++)
      {
         char character = text[i];

         if (character == '-')
            standard += "+";
         else if (character == '_')
            standard += "/";
         else if (character == '=')
            continue;
         else if ((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '+' || character == '/')
            standard += character;
         else
            return false;
      }

      // A base64 quantum is four characters; one left over is not a truncation of
      // anything and must be refused rather than silently dropped.
      int remainder = standard.GetLength() % 4;
      if (remainder == 1)
         return false;
      while (standard.GetLength() % 4 != 0)
         standard += "=";

      if (standard.IsEmpty())
         return true;

      AnsiString decoded = Base64::Decode(standard.c_str(), standard.GetLength());
      output.assign((const unsigned char*) decoded.c_str(),
                    (const unsigned char*) decoded.c_str() + decoded.GetLength());
      return true;
   }

   AnsiString
   WebPush::JsonEscape(const AnsiString &value)
   {
      AnsiString result;

      for (int i = 0; i < value.GetLength(); i++)
      {
         unsigned char character = (unsigned char) value[i];

         switch (character)
         {
         case '"':  result += "\\\""; break;
         case '\\': result += "\\\\"; break;
         case '\b': result += "\\b"; break;
         case '\f': result += "\\f"; break;
         case '\n': result += "\\n"; break;
         case '\r': result += "\\r"; break;
         case '\t': result += "\\t"; break;
         default:
            if (character < 0x20)
            {
               AnsiString escape;
               escape.Format("\\u%04x", (int) character);
               result += escape;
            }
            else
               result += (char) character;
            break;
         }
      }

      return result;
   }

   size_t
   WebPush::MaximumPlainTextLength()
   {
      return RecordSize - TagLength - 1;
   }

   bool
   WebPush::GenerateKeyPair(AnsiString &publicKey, AnsiString &privateKey, String &error)
   {
      publicKey.Empty();
      privateKey.Empty();
      error.Empty();

      EVP_PKEY *key = nullptr;
      EVP_PKEY_CTX *context = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);

      if (context == nullptr)
      {
         error = _T("OpenSSL would not open an EC key context.");
         return false;
      }

      OSSL_PARAM params[2];
      params[0] = OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, const_cast<char*>(GroupName), 0);
      params[1] = OSSL_PARAM_construct_end();

      bool ok = EVP_PKEY_keygen_init(context) == 1 &&
                EVP_PKEY_CTX_set_params(context, params) == 1 &&
                EVP_PKEY_generate(context, &key) == 1 &&
                key != nullptr;

      EVP_PKEY_CTX_free(context);

      if (!ok)
      {
         if (key != nullptr)
            EVP_PKEY_free(key);
         error = _T("OpenSSL would not generate a P-256 key pair.");
         return false;
      }

      Bytes point(PointLength, 0);
      size_t pointLength = 0;
      BIGNUM *scalar = nullptr;

      ok = EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(), &pointLength) == 1 &&
           pointLength == PointLength &&
           EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &scalar) == 1 &&
           scalar != nullptr;

      if (ok)
      {
         Bytes value(CoordinateLength, 0);
         ok = BN_bn2binpad(scalar, value.data(), (int) CoordinateLength) == (int) CoordinateLength;

         if (ok)
         {
            publicKey = Base64UrlEncode(point);
            privateKey = Base64UrlEncode(value);
         }

         OPENSSL_cleanse(value.data(), value.size());
      }

      if (scalar != nullptr)
         BN_clear_free(scalar);
      EVP_PKEY_free(key);

      if (!ok)
         error = _T("The generated key pair could not be read back in the form Web Push uses.");

      return ok;
   }

   bool
   WebPush::PublicKeyOf(const AnsiString &privateKey, AnsiString &publicKey, String &error)
   {
      publicKey.Empty();
      error.Empty();

      Bytes scalar;
      if (!Base64UrlDecode(privateKey, scalar) || scalar.size() != CoordinateLength)
      {
         error = _T("A VAPID private key is 32 octets, base64url encoded.");
         return false;
      }

      Bytes point;
      if (!PointOfScalar(scalar, point))
      {
         error = _T("That private key does not lie on P-256.");
         return false;
      }

      publicKey = Base64UrlEncode(point);
      return true;
   }

   bool
   WebPush::AudienceOf(const AnsiString &endpoint, AnsiString &audience)
   {
      audience.Empty();

      AnsiString lower = endpoint;
      lower.MakeLower();

      AnsiString scheme;
      if (lower.StartsWith("https://"))
         scheme = "https://";
      else if (lower.StartsWith("http://"))
         scheme = "http://";
      else
         return false;

      AnsiString rest = endpoint.Mid(scheme.GetLength());
      int slash = rest.Find("/");
      AnsiString host = slash < 0 ? rest : rest.Mid(0, slash);

      // A host with no dot and no colon is not a push service; an empty one is
      // not a URL at all.
      if (host.IsEmpty())
         return false;

      audience = scheme + host;
      return true;
   }

   bool
   WebPush::SignJwt(const AnsiString &payload, const AnsiString &privateKey, AnsiString &jwt, String &error)
   {
      jwt.Empty();
      error.Empty();

      Bytes scalar;
      if (!Base64UrlDecode(privateKey, scalar) || scalar.size() != CoordinateLength)
      {
         error = _T("A VAPID private key is 32 octets, base64url encoded.");
         return false;
      }

      const AnsiString header = "{\"typ\":\"JWT\",\"alg\":\"ES256\"}";
      AnsiString signingInput = Base64UrlEncode((const unsigned char*) header.c_str(), header.GetLength()) + "." +
                                Base64UrlEncode((const unsigned char*) payload.c_str(), payload.GetLength());

      EVP_PKEY *key = PrivateKeyFromScalar(scalar);
      OPENSSL_cleanse(scalar.data(), scalar.size());

      if (key == nullptr)
      {
         error = _T("That private key does not lie on P-256.");
         return false;
      }

      EVP_MD_CTX *context = EVP_MD_CTX_new();
      Bytes der;
      size_t length = 0;

      bool ok = context != nullptr &&
                EVP_DigestSignInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                EVP_DigestSign(context, nullptr, &length, (const unsigned char*) signingInput.c_str(),
                               signingInput.GetLength()) == 1 &&
                length > 0;

      if (ok)
      {
         der.assign(length, 0);
         ok = EVP_DigestSign(context, der.data(), &length, (const unsigned char*) signingInput.c_str(),
                             signingInput.GetLength()) == 1;
         der.resize(length);
      }

      Bytes raw;
      ok = ok && DerToRawSignature(der, raw);

      if (context != nullptr)
         EVP_MD_CTX_free(context);
      EVP_PKEY_free(key);

      if (!ok)
      {
         error = _T("The VAPID token could not be signed.");
         return false;
      }

      jwt = signingInput + "." + Base64UrlEncode(raw);
      return true;
   }

   bool
   WebPush::BuildVapidHeader(const AnsiString &endpoint, const AnsiString &subject, __int64 expiry,
                             const AnsiString &publicKey, const AnsiString &privateKey,
                             AnsiString &header, String &error)
   {
      header.Empty();
      error.Empty();

      AnsiString audience;
      if (!AudienceOf(endpoint, audience))
      {
         error = _T("The push endpoint is not an http or https URL, so there is no audience to sign for.");
         return false;
      }

      AnsiString claims;
      claims.Format("{\"aud\":\"%hs\",\"exp\":%I64d", JsonEscape(audience).c_str(), expiry);

      // sub is optional in RFC 8292 and every push service in the field wants it:
      // it is the only way they can tell a sender that its subscriptions are
      // failing. Left out when it is not configured rather than sent empty, which
      // some services refuse.
      if (!subject.IsEmpty())
         claims += ",\"sub\":\"" + JsonEscape(subject) + "\"";

      claims += "}";

      AnsiString jwt;
      if (!SignJwt(claims, privateKey, jwt, error))
         return false;

      header = "vapid t=" + jwt + ", k=" + publicKey;
      return true;
   }

   bool
   WebPush::Encrypt(const AnsiString &plainText, const AnsiString &receiverPublicKey,
                    const AnsiString &authSecret, AnsiString &body, String &error)
   {
      AnsiString senderPublic;
      AnsiString senderPrivate;
      if (!GenerateKeyPair(senderPublic, senderPrivate, error))
         return false;

      Bytes salt(SaltLength, 0);
      if (RAND_bytes(salt.data(), (int) salt.size()) != 1)
      {
         error = _T("OpenSSL would not produce a salt for the push message.");
         return false;
      }

      return EncryptWith(plainText, receiverPublicKey, authSecret, senderPrivate, Base64UrlEncode(salt), body, error);
   }

   bool
   WebPush::EncryptWith(const AnsiString &plainText, const AnsiString &receiverPublicKey,
                        const AnsiString &authSecret, const AnsiString &senderPrivateKey,
                        const AnsiString &salt, AnsiString &body, String &error)
   {
      body.Empty();
      error.Empty();

      if ((size_t) plainText.GetLength() > MaximumPlainTextLength())
      {
         error = _T("A push message must fit in one record, and this one does not.");
         return false;
      }

      Bytes receiverPoint;
      if (!Base64UrlDecode(receiverPublicKey, receiverPoint) || receiverPoint.size() != PointLength)
      {
         error = _T("The subscription's p256dh key is not a 65-octet P-256 point.");
         return false;
      }

      Bytes auth;
      if (!Base64UrlDecode(authSecret, auth) || auth.empty())
      {
         error = _T("The subscription's auth secret is not base64url, or is empty.");
         return false;
      }

      Bytes saltBytes;
      if (!Base64UrlDecode(salt, saltBytes) || saltBytes.size() != SaltLength)
      {
         error = _T("A push message salt is 16 octets.");
         return false;
      }

      Bytes senderScalar;
      if (!Base64UrlDecode(senderPrivateKey, senderScalar) || senderScalar.size() != CoordinateLength)
      {
         error = _T("The sending key is not a 32-octet P-256 scalar.");
         return false;
      }

      Bytes senderPoint;
      if (!PointOfScalar(senderScalar, senderPoint))
      {
         error = _T("The sending key does not lie on P-256.");
         return false;
      }

      EVP_PKEY *own = PrivateKeyFromScalar(senderScalar);
      EVP_PKEY *peer = PublicKeyFromPoint(receiverPoint);
      OPENSSL_cleanse(senderScalar.data(), senderScalar.size());

      Bytes secret;
      bool ok = own != nullptr && peer != nullptr && SharedSecret(own, peer, secret);

      if (own != nullptr)
         EVP_PKEY_free(own);
      if (peer != nullptr)
         EVP_PKEY_free(peer);

      if (!ok)
      {
         error = _T("The key agreement with the subscription's key failed.");
         return false;
      }

      Bytes key;
      Bytes nonce;
      ok = DeriveKeyAndNonce(secret, auth, receiverPoint, senderPoint, saltBytes, key, nonce);
      OPENSSL_cleanse(secret.data(), secret.size());

      if (!ok)
      {
         error = _T("The content encryption key could not be derived.");
         return false;
      }

      // One record: the plaintext, then the padding delimiter 0x02 that RFC 8188
      // uses for the last record. There is no padding beyond it, and RFC 8291
      // section 4 is explicit that a sender may add some; this one does not, and
      // the documentation says so, because padding hides a length that the push
      // service can see anyway from the record size.
      Bytes payload((const unsigned char*) plainText.c_str(),
                    (const unsigned char*) plainText.c_str() + plainText.GetLength());
      payload.push_back(0x02);

      Bytes cipherText;
      ok = AesGcmSeal(key, nonce, payload, cipherText);
      OPENSSL_cleanse(key.data(), key.size());

      if (!ok)
      {
         error = _T("AES-128-GCM refused to encrypt the push message.");
         return false;
      }

      Bytes framed;
      Append(framed, saltBytes);
      framed.push_back((unsigned char) ((RecordSize >> 24) & 0xff));
      framed.push_back((unsigned char) ((RecordSize >> 16) & 0xff));
      framed.push_back((unsigned char) ((RecordSize >> 8) & 0xff));
      framed.push_back((unsigned char) (RecordSize & 0xff));
      framed.push_back((unsigned char) senderPoint.size());
      Append(framed, senderPoint);
      Append(framed, cipherText);

      body.Empty();
      body.append((const char*) framed.data(), framed.size());
      return true;
   }

   bool
   WebPush::Decrypt(const AnsiString &body, const AnsiString &receiverPrivateKey,
                    const AnsiString &authSecret, AnsiString &plainText, String &error)
   {
      plainText.Empty();
      error.Empty();

      const size_t HeaderLength = SaltLength + 4 + 1;

      Bytes framed((const unsigned char*) body.c_str(),
                   (const unsigned char*) body.c_str() + body.GetLength());

      if (framed.size() < HeaderLength)
      {
         error = _T("The body is shorter than an aes128gcm header.");
         return false;
      }

      Bytes salt(framed.begin(), framed.begin() + SaltLength);
      size_t keyIdLength = framed[SaltLength + 4];

      if (keyIdLength != PointLength || framed.size() < HeaderLength + keyIdLength + TagLength)
      {
         error = _T("The body does not carry a 65-octet sender key and a record.");
         return false;
      }

      Bytes senderPoint(framed.begin() + HeaderLength, framed.begin() + HeaderLength + keyIdLength);
      Bytes cipherText(framed.begin() + HeaderLength + keyIdLength, framed.end());

      Bytes scalar;
      if (!Base64UrlDecode(receiverPrivateKey, scalar) || scalar.size() != CoordinateLength)
      {
         error = _T("The receiving key is not a 32-octet P-256 scalar.");
         return false;
      }

      Bytes receiverPoint;
      if (!PointOfScalar(scalar, receiverPoint))
      {
         error = _T("The receiving key does not lie on P-256.");
         return false;
      }

      Bytes auth;
      if (!Base64UrlDecode(authSecret, auth) || auth.empty())
      {
         error = _T("The auth secret is not base64url, or is empty.");
         return false;
      }

      EVP_PKEY *own = PrivateKeyFromScalar(scalar);
      EVP_PKEY *peer = PublicKeyFromPoint(senderPoint);
      OPENSSL_cleanse(scalar.data(), scalar.size());

      Bytes secret;
      bool ok = own != nullptr && peer != nullptr && SharedSecret(own, peer, secret);

      if (own != nullptr)
         EVP_PKEY_free(own);
      if (peer != nullptr)
         EVP_PKEY_free(peer);

      if (!ok)
      {
         error = _T("The key agreement with the sender's key failed.");
         return false;
      }

      Bytes key;
      Bytes nonce;
      ok = DeriveKeyAndNonce(secret, auth, receiverPoint, senderPoint, salt, key, nonce);
      OPENSSL_cleanse(secret.data(), secret.size());

      Bytes payload;
      ok = ok && AesGcmOpen(key, nonce, cipherText, payload);

      if (!key.empty())
         OPENSSL_cleanse(key.data(), key.size());

      if (!ok)
      {
         error = _T("The record did not open: the keys do not match, or the body was changed.");
         return false;
      }

      // Strip the padding: the last 0x02 delimiter and every zero after it.
      size_t end = payload.size();
      while (end > 0 && payload[end - 1] == 0x00)
         end--;

      if (end == 0 || payload[end - 1] != 0x02)
      {
         error = _T("The record has no padding delimiter, so it is not a single-record push message.");
         return false;
      }

      end--;
      plainText.append((const char*) payload.data(), end);
      return true;
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   void
   WebPushTester::Test()
   {
      // RFC 8291 section 5 and appendix A. Every input is the RFC's and the body
      // produced must be the body the RFC prints - so this proves the derivation,
      // the framing and the cipher against the specification, not against another
      // copy of this code.
      const AnsiString plainText = "When I grow up, I want to be a watermelon";
      const AnsiString uaPublic = "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4";
      const AnsiString uaPrivate = "q1dXpw3UpT5VOmu_cf_v6ih07Aems3njxI-JWgLcM94";
      const AnsiString asPublic = "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8";
      const AnsiString asPrivate = "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw";
      const AnsiString salt = "DGv6ra1nlYgDCS1FRnbzlw";
      const AnsiString auth = "BTBZMqHH6r4Tts7J_aSIgg";
      const AnsiString expected =
         "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6Tlz"
         "AC8wEqKK6PBru3jl7A_yl95bQpu6cVPTpK4Mqgkf1CXztLVBSt2Ks3oZwbuwXPXLWyouBWLVWGNWQexSgSxsj_Qulcy4a-fN";

      AnsiString body;
      String error;

      if (!WebPush::EncryptWith(plainText, uaPublic, auth, asPrivate, salt, body, error))
      {
         AnsiString refusal = "WebPush::EncryptWith refused the worked example of RFC 8291: " + AnsiString(error);
         throw std::logic_error(refusal.c_str());
      }

      AnsiString produced = WebPush::Base64UrlEncode((const unsigned char*) body.c_str(), body.GetLength());
      if (produced != expected)
         throw std::logic_error("WebPush::EncryptWith did not reproduce the push message of RFC 8291 section 5.");

      // The sender's public key is the key id in the header, so a private key
      // whose public half is derived wrongly would produce a body nothing could
      // read. The RFC prints both halves; they must agree.
      AnsiString derivedPublic;
      if (!WebPush::PublicKeyOf(asPrivate, derivedPublic, error) || derivedPublic != asPublic)
         throw std::logic_error("WebPush::PublicKeyOf did not derive the application server public key of RFC 8291 from its private key.");

      // And the other direction, with the receiver's private key the RFC gives.
      AnsiString recovered;
      if (!WebPush::Decrypt(body, uaPrivate, auth, recovered, error) || recovered != plainText)
         throw std::logic_error("WebPush::Decrypt did not recover the plaintext of RFC 8291 section 5.");

      // A round trip on keys nothing published, so that key generation itself is
      // exercised: what comes back must be what went in, and the body must differ
      // between two encryptions of the same text (a fresh salt and key each time).
      AnsiString devicePublic;
      AnsiString devicePrivate;
      if (!WebPush::GenerateKeyPair(devicePublic, devicePrivate, error))
         throw std::logic_error("WebPush::GenerateKeyPair could not make a P-256 key pair.");

      AnsiString deviceAuth = WebPush::Base64UrlEncode((const unsigned char*) "0123456789abcdef", 16);
      AnsiString first;
      AnsiString second;
      const AnsiString message = "{\"kind\":\"mail\",\"from\":\"a@example.com\"}";

      if (!WebPush::Encrypt(message, devicePublic, deviceAuth, first, error) ||
          !WebPush::Encrypt(message, devicePublic, deviceAuth, second, error))
         throw std::logic_error("WebPush::Encrypt could not encrypt to a generated subscription key.");

      if (first == second)
         throw std::logic_error("WebPush::Encrypt produced the same body twice; the salt or the ephemeral key is not fresh.");

      AnsiString back;
      if (!WebPush::Decrypt(first, devicePrivate, deviceAuth, back, error) || back != message)
         throw std::logic_error("A push message encrypted to a generated key did not decrypt back to what went in.");

      if (WebPush::Decrypt(second, uaPrivate, auth, back, error))
         throw std::logic_error("A push message decrypted under the wrong subscription key.");

      // The VAPID header: the shape RFC 8292 defines, and a signature that
      // verifies under the public key the header names.
      AnsiString vapidPublic;
      AnsiString vapidPrivate;
      if (!WebPush::GenerateKeyPair(vapidPublic, vapidPrivate, error))
         throw std::logic_error("WebPush::GenerateKeyPair could not make a VAPID key pair.");

      AnsiString header;
      if (!WebPush::BuildVapidHeader("https://push.example.net/push/abc", "mailto:postmaster@example.com",
                                     (__int64) 1789000000, vapidPublic, vapidPrivate, header, error))
         throw std::logic_error("WebPush::BuildVapidHeader refused to build a header.");

      AnsiString keyPart = ", k=" + vapidPublic;
      if (!header.StartsWith("vapid t=") || header.Find(keyPart.c_str()) < 0)
         throw std::logic_error("The VAPID header is not \"vapid t=<jwt>, k=<public key>\".");

      AnsiString token = header.Mid(8, header.Find(", k=") - 8);
      int firstDot = token.Find(".");
      int secondDot = firstDot < 0 ? -1 : token.Find(".", firstDot + 1);
      if (firstDot < 0 || secondDot < 0)
         throw std::logic_error("The VAPID token is not three base64url parts.");

      std::vector<unsigned char> claims;
      if (!WebPush::Base64UrlDecode(token.Mid(firstDot + 1, secondDot - firstDot - 1), claims))
         throw std::logic_error("The VAPID token's claims are not base64url.");

      AnsiString claimText;
      claimText.append((const char*) claims.data(), claims.size());
      if (claimText.Find("\"aud\":\"https://push.example.net\"") < 0)
         throw std::logic_error("The VAPID claims do not name the endpoint's origin as the audience.");
      if (claimText.Find("\"sub\":\"mailto:postmaster@example.com\"") < 0)
         throw std::logic_error("The VAPID claims do not carry the configured subject.");
      if (claimText.Find("\"exp\":1789000000") < 0)
         throw std::logic_error("The VAPID claims do not carry the expiry they were given.");

      // The signature, verified the way a push service would: over
      // "<header>.<claims>", with r and s read as two fixed-width halves.
      std::vector<unsigned char> raw;
      std::vector<unsigned char> point;
      if (!WebPush::Base64UrlDecode(token.Mid(secondDot + 1), raw) || raw.size() != 64 ||
          !WebPush::Base64UrlDecode(vapidPublic, point) || point.size() != 65)
         throw std::logic_error("The VAPID signature is not 64 octets, or the public key is not a P-256 point.");

      AnsiString signingInput = token.Mid(0, secondDot);

      EVP_PKEY *key = PublicKeyFromPoint(point);
      if (key == nullptr)
         throw std::logic_error("The VAPID public key in the header is not a point on P-256.");

      ECDSA_SIG *signature = ECDSA_SIG_new();
      BIGNUM *r = BN_bin2bn(raw.data(), 32, nullptr);
      BIGNUM *s = BN_bin2bn(raw.data() + 32, 32, nullptr);
      std::vector<unsigned char> der;

      bool verified = false;
      if (signature != nullptr && r != nullptr && s != nullptr && ECDSA_SIG_set0(signature, r, s) == 1)
      {
         int length = i2d_ECDSA_SIG(signature, nullptr);
         if (length > 0)
         {
            der.assign((size_t) length, 0);
            unsigned char *pointer = der.data();
            i2d_ECDSA_SIG(signature, &pointer);

            EVP_MD_CTX *context = EVP_MD_CTX_new();
            verified = context != nullptr &&
                       EVP_DigestVerifyInit(context, nullptr, EVP_sha256(), nullptr, key) == 1 &&
                       EVP_DigestVerify(context, der.data(), der.size(),
                                        (const unsigned char*) signingInput.c_str(), signingInput.GetLength()) == 1;
            if (context != nullptr)
               EVP_MD_CTX_free(context);
         }
      }

      if (signature != nullptr)
         ECDSA_SIG_free(signature);
      EVP_PKEY_free(key);

      if (!verified)
         throw std::logic_error("The VAPID token's signature does not verify under the public key the header names.");

      // A subscription whose keys are the wrong length is refused before anything
      // is derived; a push service would otherwise be sent a body nobody can read.
      if (WebPush::Encrypt(message, "not-a-key", deviceAuth, first, error))
         throw std::logic_error("WebPush::Encrypt accepted a p256dh key that is not a point.");
      if (WebPush::Encrypt(message, devicePublic, "", first, error))
         throw std::logic_error("WebPush::Encrypt accepted an empty auth secret.");

      AnsiString audience;
      if (!WebPush::AudienceOf("https://fcm.googleapis.com/fcm/send/abc:def", audience) ||
          audience != "https://fcm.googleapis.com")
         throw std::logic_error("WebPush::AudienceOf did not reduce an endpoint to its origin.");
      if (WebPush::AudienceOf("ftp://push.example.net/x", audience))
         throw std::logic_error("WebPush::AudienceOf accepted an endpoint that is not http or https.");
   }
}
