// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The SNMPv3 USM cryptography: HMAC-SHA-256-192, AES-128-CFB and the key
// localisation. See SnmpUsm.h.

#include "StdAfx.h"

#include "SnmpUsm.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstring>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const size_t SnmpUsm::KeyLength;
   const size_t SnmpUsm::AuthParameterLength;
   const size_t SnmpUsm::PrivKeyLength;
   const size_t SnmpUsm::PrivParameterLength;
   const size_t SnmpUsm::MinimumPasswordLength;

   namespace
   {
      // RFC 3414 A.2: the password repeated to fill one megabyte.
      const size_t PasswordToKeyOctets = 1048576;
   }

   bool
   SnmpUsm::PasswordToKey(const std::string &password, SnmpBytes &key)
   {
      key.clear();

      if (password.size() < MinimumPasswordLength)
         return false;

      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (!context)
         return false;

      bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;

      // Sixty-four octets at a time, as the RFC's own code does, each the next
      // stretch of the password repeated end to end.
      unsigned char chunk[64];
      size_t index = 0;

      for (size_t done = 0; ok && done < PasswordToKeyOctets; done += sizeof(chunk))
      {
         for (size_t i = 0; i < sizeof(chunk); i++)
            chunk[i] = (unsigned char) password[index++ % password.size()];

         ok = EVP_DigestUpdate(context, chunk, sizeof(chunk)) == 1;
      }

      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int digestLength = 0;

      if (ok)
         ok = EVP_DigestFinal_ex(context, digest, &digestLength) == 1 && digestLength == KeyLength;

      EVP_MD_CTX_free(context);
      OPENSSL_cleanse(chunk, sizeof(chunk));

      if (!ok)
      {
         OPENSSL_cleanse(digest, sizeof(digest));
         return false;
      }

      key.assign(digest, digest + digestLength);
      OPENSSL_cleanse(digest, sizeof(digest));
      return true;
   }

   bool
   SnmpUsm::LocaliseKey(const SnmpBytes &key, const SnmpBytes &engineId, SnmpBytes &localised)
   {
      localised.clear();

      if (key.size() != KeyLength || engineId.empty())
         return false;

      SnmpBytes material;
      material.reserve(key.size() * 2 + engineId.size());
      material.insert(material.end(), key.begin(), key.end());
      material.insert(material.end(), engineId.begin(), engineId.end());
      material.insert(material.end(), key.begin(), key.end());

      const bool ok = Sha256(&material[0], material.size(), localised);

      Scrub(material);
      return ok;
   }

   bool
   SnmpUsm::LocalisedKey(const std::string &password, const SnmpBytes &engineId, SnmpBytes &localised)
   {
      SnmpBytes key;
      if (!PasswordToKey(password, key))
         return false;

      const bool ok = LocaliseKey(key, engineId, localised);
      Scrub(key);
      return ok;
   }

   bool
   SnmpUsm::Digest(const SnmpBytes &authKey, const unsigned char *message, size_t length,
                   unsigned char digest[AuthParameterLength])
   {
      if (authKey.size() != KeyLength || !message || length == 0)
         return false;

      unsigned char full[EVP_MAX_MD_SIZE];
      unsigned int fullLength = 0;

      if (HMAC(EVP_sha256(), &authKey[0], (int) authKey.size(), message, length, full, &fullLength) == nullptr ||
          fullLength < AuthParameterLength)
      {
         OPENSSL_cleanse(full, sizeof(full));
         return false;
      }

      memcpy(digest, full, AuthParameterLength);
      OPENSSL_cleanse(full, sizeof(full));
      return true;
   }

   bool
   SnmpUsm::DigestsEqual(const unsigned char *left, const unsigned char *right, size_t length)
   {
      if (!left || !right)
         return false;

      return CRYPTO_memcmp(left, right, length) == 0;
   }

   void
   SnmpUsm::MakeIv(unsigned int boots, unsigned int engineTime, const unsigned char salt[PrivParameterLength],
                   unsigned char iv[16])
   {
      iv[0] = (unsigned char) ((boots >> 24) & 0xFF);
      iv[1] = (unsigned char) ((boots >> 16) & 0xFF);
      iv[2] = (unsigned char) ((boots >> 8) & 0xFF);
      iv[3] = (unsigned char) (boots & 0xFF);
      iv[4] = (unsigned char) ((engineTime >> 24) & 0xFF);
      iv[5] = (unsigned char) ((engineTime >> 16) & 0xFF);
      iv[6] = (unsigned char) ((engineTime >> 8) & 0xFF);
      iv[7] = (unsigned char) (engineTime & 0xFF);

      for (size_t i = 0; i < PrivParameterLength; i++)
         iv[8 + i] = salt[i];
   }

   bool
   SnmpUsm::Encrypt(const SnmpBytes &privKey, const unsigned char iv[16],
                    const unsigned char *data, size_t length, SnmpBytes &out)
   {
      out.clear();

      if (privKey.size() < PrivKeyLength)
         return false;

      if (length == 0)
         return true;

      if (!data || length > 0x7FFFFFFF)
         return false;

      EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
      if (!context)
         return false;

      out.resize(length);
      int written = 0;
      int final = 0;

      const bool ok = EVP_EncryptInit_ex(context, EVP_aes_128_cfb128(), nullptr, &privKey[0], iv) == 1 &&
                      EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
                      EVP_EncryptUpdate(context, &out[0], &written, data, (int) length) == 1 &&
                      EVP_EncryptFinal_ex(context, &out[0] + written, &final) == 1 &&
                      (size_t) (written + final) == length;

      EVP_CIPHER_CTX_free(context);

      if (!ok)
         out.clear();

      return ok;
   }

   bool
   SnmpUsm::Decrypt(const SnmpBytes &privKey, const unsigned char iv[16],
                    const unsigned char *data, size_t length, SnmpBytes &out)
   {
      out.clear();

      if (privKey.size() < PrivKeyLength)
         return false;

      if (length == 0)
         return true;

      if (!data || length > 0x7FFFFFFF)
         return false;

      EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
      if (!context)
         return false;

      out.resize(length);
      int written = 0;
      int final = 0;

      const bool ok = EVP_DecryptInit_ex(context, EVP_aes_128_cfb128(), nullptr, &privKey[0], iv) == 1 &&
                      EVP_CIPHER_CTX_set_padding(context, 0) == 1 &&
                      EVP_DecryptUpdate(context, &out[0], &written, data, (int) length) == 1 &&
                      EVP_DecryptFinal_ex(context, &out[0] + written, &final) == 1 &&
                      (size_t) (written + final) == length;

      EVP_CIPHER_CTX_free(context);

      if (!ok)
      {
         Scrub(out);
         out.clear();
      }

      return ok;
   }

   bool
   SnmpUsm::Sha256(const unsigned char *data, size_t length, SnmpBytes &digest)
   {
      digest.clear();

      unsigned char md[EVP_MAX_MD_SIZE];
      unsigned int mdLength = 0;

      static const unsigned char nothing = 0;

      if (EVP_Digest(data ? data : &nothing, data ? length : 0, md, &mdLength, EVP_sha256(), nullptr) != 1)
         return false;

      digest.assign(md, md + mdLength);
      OPENSSL_cleanse(md, sizeof(md));
      return true;
   }

   void
   SnmpUsm::Scrub(SnmpBytes &bytes)
   {
      if (!bytes.empty())
         OPENSSL_cleanse(&bytes[0], bytes.size());

      bytes.clear();
   }

   void
   SnmpUsm::Scrub(std::string &text)
   {
      if (!text.empty())
         OPENSSL_cleanse(&text[0], text.size());

      text.clear();
   }
}
