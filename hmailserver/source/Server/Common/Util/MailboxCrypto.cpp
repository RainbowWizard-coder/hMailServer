// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "MailboxCrypto.h"
#include "Assert.h"
#include "Hashing/HashCreator.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/sha.h>

#include <cstdlib>
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
      const char *Version = "hmk1";

      // The HKDF info of a message's stage: what the derived key is for, so a
      // key derived for a stored message is never one for anything else.
      const char *StageInfo = "hMailServer message store stage 3 v1";

      std::string FromHexLiteral(const char *hex)
      {
         std::string out;
         const size_t length = strlen(hex);
         for (size_t i = 0; i + 1 < length; i += 2)
         {
            char pair[3] = { hex[i], hex[i + 1], 0 };
            out.push_back((char) strtol(pair, nullptr, 16));
         }
         return out;
      }

      // The fields of a text form, split on '$'.
      std::vector<std::string> Fields(const std::string &text)
      {
         std::vector<std::string> fields;
         size_t start = 0;
         for (;;)
         {
            const size_t at = text.find('$', start);
            if (at == std::string::npos)
            {
               fields.push_back(text.substr(start));
               break;
            }
            fields.push_back(text.substr(start, at - start));
            start = at + 1;
         }
         return fields;
      }

      bool Number(const std::string &text, int minimum, int maximum, int &value)
      {
         if (text.empty() || text.size() > 9)
            return false;
         for (size_t i = 0; i < text.size(); i++)
         {
            if (text[i] < '0' || text[i] > '9')
               return false;
         }
         value = atoi(text.c_str());
         return value >= minimum && value <= maximum;
      }

      std::string NumberText(int value)
      {
         char buffer[16];
         sprintf_s(buffer, sizeof(buffer), "%d", value);
         return std::string(buffer);
      }

      bool DeriveArgon2(const AnsiString &secret, const std::string &salt, uint32_t memoryKiB, uint32_t passes, uint32_t lanes, std::string &key)
      {
         key.clear();
#if OPENSSL_VERSION_NUMBER < 0x30200000L
         (void) secret; (void) salt; (void) memoryKiB; (void) passes; (void) lanes;
         return false;
#else
         EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
         if (kdf == nullptr)
            return false;

         EVP_KDF_CTX *context = EVP_KDF_CTX_new(kdf);
         EVP_KDF_free(kdf);
         if (context == nullptr)
            return false;

         uint32_t threads = 1;
         OSSL_PARAM params[7];
         OSSL_PARAM *p = params;
         *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, (void *) secret.c_str(), (size_t) secret.GetLength());
         *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *) salt.data(), salt.size());
         *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes);
         *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads);
         *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memoryKiB);
         *p++ = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &passes);
         *p = OSSL_PARAM_construct_end();

         unsigned char out[MailboxCrypto::KeyBytes] = {};
         const int result = EVP_KDF_derive(context, out, sizeof(out), params);
         EVP_KDF_CTX_free(context);

         if (result != 1)
         {
            OPENSSL_cleanse(out, sizeof(out));
            return false;
         }

         key.assign((const char *) out, sizeof(out));
         OPENSSL_cleanse(out, sizeof(out));
         return true;
#endif
      }

      bool DerivePbkdf2(const AnsiString &secret, const std::string &salt, int iterations, std::string &key)
      {
         key.clear();
         unsigned char out[MailboxCrypto::KeyBytes] = {};
         if (PKCS5_PBKDF2_HMAC(secret.c_str(), secret.GetLength(), (const unsigned char *) salt.data(), (int) salt.size(),
                               iterations, EVP_sha256(), (int) sizeof(out), out) != 1)
            return false;

         key.assign((const char *) out, sizeof(out));
         OPENSSL_cleanse(out, sizeof(out));
         return true;
      }

      // The associated data of a seal to a public key: the purpose, then the
      // two public keys, so a copy cannot be moved to another recipient or
      // read as another kind of copy.
      std::string SealAad(const std::string &purpose, const std::string &recipientPublic, const std::string &ephemeralPublic)
      {
         return std::string("hMailServer sealed ") + purpose + std::string(1, '\0') + recipientPublic + ephemeralPublic;
      }

      std::string SealKey(const std::string &shared, const std::string &recipientPublic, const std::string &ephemeralPublic, const std::string &info)
      {
         return MailboxCrypto::Hkdf(shared, ephemeralPublic + recipientPublic, info, MailboxCrypto::KeyBytes);
      }
   }

   bool
   MailboxCrypto::Argon2Available()
   {
      return HashCreator::Argon2idAvailable();
   }

   std::string
   MailboxCrypto::RandomBytes(size_t count)
   {
      std::string out(count, '\0');
      if (count == 0)
         return out;

      if (RAND_bytes((unsigned char *) &out[0], (int) count) != 1)
         return std::string();

      return out;
   }

   void
   MailboxCrypto::Forget(std::string &secret) noexcept
   {
      if (!secret.empty())
         OPENSSL_cleanse(&secret[0], secret.size());
      secret.clear();
   }

   bool
   MailboxCrypto::ConstantTimeEquals(const std::string &one, const std::string &two)
   {
      if (one.size() != two.size())
         return false;
      if (one.empty())
         return true;
      return CRYPTO_memcmp(one.data(), two.data(), one.size()) == 0;
   }

   std::string
   MailboxCrypto::Base64(const std::string &bytes)
   {
      if (bytes.empty())
         return std::string();

      std::vector<unsigned char> out(4 * ((bytes.size() + 2) / 3) + 1, 0);
      const int written = EVP_EncodeBlock(&out[0], (const unsigned char *) bytes.data(), (int) bytes.size());
      if (written < 0)
         return std::string();

      return std::string((const char *) &out[0], (size_t) written);
   }

   bool
   MailboxCrypto::FromBase64(const std::string &text, std::string &bytes)
   {
      bytes.clear();

      if (text.empty())
         return true;

      if (text.size() % 4 != 0)
         return false;

      for (size_t i = 0; i < text.size(); i++)
      {
         const char c = text[i];
         const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
                         (c == '=' && i >= text.size() - 2);
         if (!ok)
            return false;
      }

      std::vector<unsigned char> out(3 * (text.size() / 4) + 1, 0);
      const int written = EVP_DecodeBlock(&out[0], (const unsigned char *) text.data(), (int) text.size());
      if (written < 0)
         return false;

      // EVP_DecodeBlock counts the padding as bytes of zeros.
      size_t length = (size_t) written;
      if (text[text.size() - 1] == '=')
         length--;
      if (text.size() >= 2 && text[text.size() - 2] == '=')
         length--;

      bytes.assign((const char *) &out[0], length);
      OPENSSL_cleanse(&out[0], out.size());
      return true;
   }

   AnsiString
   MailboxCrypto::Fingerprint(const std::string &publicKey)
   {
      unsigned char digest[SHA256_DIGEST_LENGTH] = {};
      SHA256((const unsigned char *) publicKey.data(), publicKey.size(), digest);

      static const char *digits = "0123456789abcdef";
      AnsiString hex;
      for (int i = 0; i < 8; i++)
      {
         hex += digits[digest[i] >> 4];
         hex += digits[digest[i] & 0x0F];
      }
      return hex;
   }

   std::string
   MailboxCrypto::Hkdf(const std::string &inputKey, const std::string &salt, const std::string &info, size_t length)
   {
      if (length == 0 || length > 32)
         return std::string();

      unsigned char prk[32] = {};
      unsigned int prkLength = 0;

      // RFC 5869 2.2: a salt of no bytes is a string of HashLen zeros.
      const std::string usedSalt = salt.empty() ? std::string(32, '\0') : salt;
      if (!HMAC(EVP_sha256(), usedSalt.data(), (int) usedSalt.size(), (const unsigned char *) inputKey.data(), inputKey.size(), prk, &prkLength) ||
          prkLength != 32)
      {
         OPENSSL_cleanse(prk, sizeof(prk));
         return std::string();
      }

      // One block of the expansion is all a 32-byte key needs: T(1).
      std::string block = info;
      block.push_back((char) 1);

      unsigned char okm[32] = {};
      unsigned int okmLength = 0;
      const bool expanded = HMAC(EVP_sha256(), prk, (int) prkLength, (const unsigned char *) block.data(), block.size(), okm, &okmLength) != nullptr &&
                            okmLength == 32;
      OPENSSL_cleanse(prk, sizeof(prk));

      if (!expanded)
      {
         OPENSSL_cleanse(okm, sizeof(okm));
         return std::string();
      }

      std::string out((const char *) okm, length);
      OPENSSL_cleanse(okm, sizeof(okm));
      return out;
   }

   bool
   MailboxCrypto::GenerateKeyPair(std::string &privateKey, std::string &publicKey)
   {
      privateKey.clear();
      publicKey.clear();

      EVP_PKEY_CTX *generate = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
      EVP_PKEY *key = nullptr;
      bool ok = generate && EVP_PKEY_keygen_init(generate) == 1 && EVP_PKEY_keygen(generate, &key) == 1 && key;

      unsigned char privateBytes[32] = {};
      unsigned char publicBytes[32] = {};
      size_t privateLength = sizeof(privateBytes);
      size_t publicLength = sizeof(publicBytes);

      ok = ok && EVP_PKEY_get_raw_private_key(key, privateBytes, &privateLength) == 1 && privateLength == 32;
      ok = ok && EVP_PKEY_get_raw_public_key(key, publicBytes, &publicLength) == 1 && publicLength == 32;

      if (ok)
      {
         privateKey.assign((const char *) privateBytes, 32);
         publicKey.assign((const char *) publicBytes, 32);
      }

      OPENSSL_cleanse(privateBytes, sizeof(privateBytes));
      if (key)
         EVP_PKEY_free(key);
      if (generate)
         EVP_PKEY_CTX_free(generate);

      return ok;
   }

   std::string
   MailboxCrypto::PublicKeyOf(const std::string &privateKey)
   {
      if (privateKey.size() != 32)
         return std::string();

      EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, (const unsigned char *) privateKey.data(), privateKey.size());
      if (!key)
         return std::string();

      unsigned char publicBytes[32] = {};
      size_t publicLength = sizeof(publicBytes);
      const bool ok = EVP_PKEY_get_raw_public_key(key, publicBytes, &publicLength) == 1 && publicLength == 32;
      EVP_PKEY_free(key);

      return ok ? std::string((const char *) publicBytes, 32) : std::string();
   }

   std::string
   MailboxCrypto::Agree(const std::string &privateKey, const std::string &peerPublic)
   {
      if (privateKey.size() != 32 || peerPublic.size() != 32)
         return std::string();

      EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, (const unsigned char *) privateKey.data(), privateKey.size());
      EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, (const unsigned char *) peerPublic.data(), peerPublic.size());
      EVP_PKEY_CTX *derive = mine ? EVP_PKEY_CTX_new(mine, nullptr) : nullptr;

      unsigned char shared[32] = {};
      size_t sharedLength = sizeof(shared);
      const bool ok = derive && theirs && EVP_PKEY_derive_init(derive) == 1 && EVP_PKEY_derive_set_peer(derive, theirs) == 1 &&
                      EVP_PKEY_derive(derive, shared, &sharedLength) == 1 && sharedLength == 32;

      if (derive)
         EVP_PKEY_CTX_free(derive);
      if (theirs)
         EVP_PKEY_free(theirs);
      if (mine)
         EVP_PKEY_free(mine);

      if (!ok)
      {
         OPENSSL_cleanse(shared, sizeof(shared));
         return std::string();
      }

      // An all-zero agreement is what a low-order point gives (RFC 7748
      // section 6.1): refused, as the RFC recommends.
      unsigned char any = 0;
      for (size_t i = 0; i < sizeof(shared); i++)
         any |= shared[i];
      if (any == 0)
         return std::string();

      std::string out((const char *) shared, 32);
      OPENSSL_cleanse(shared, sizeof(shared));
      return out;
   }

   bool
   MailboxCrypto::Encrypt(const std::string &key, const std::string &nonce, const std::string &aad, const std::string &plain,
                          std::string &cipher, std::string &tag)
   {
      cipher.clear();
      tag.clear();

      if (key.size() != KeyBytes || nonce.size() != NonceBytes || plain.size() > 0x7FFFFFF0)
         return false;

      EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
      if (!context)
         return false;

      std::vector<unsigned char> out(plain.size() + 16, 0);
      unsigned char tagBytes[TagBytes] = {};
      int length = 0;
      int total = 0;

      bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
      ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int) NonceBytes, nullptr) == 1;
      ok = ok && EVP_EncryptInit_ex(context, nullptr, nullptr, (const unsigned char *) key.data(), (const unsigned char *) nonce.data()) == 1;
      if (ok && !aad.empty())
         ok = EVP_EncryptUpdate(context, nullptr, &length, (const unsigned char *) aad.data(), (int) aad.size()) == 1;
      if (ok && !plain.empty())
      {
         ok = EVP_EncryptUpdate(context, &out[0], &length, (const unsigned char *) plain.data(), (int) plain.size()) == 1;
         total = length;
      }
      ok = ok && EVP_EncryptFinal_ex(context, &out[0] + total, &length) == 1;
      total += ok ? length : 0;
      ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, (int) TagBytes, tagBytes) == 1;
      EVP_CIPHER_CTX_free(context);

      if (!ok || (size_t) total != plain.size())
         return false;

      cipher.assign((const char *) &out[0], (size_t) total);
      tag.assign((const char *) tagBytes, TagBytes);
      return true;
   }

   bool
   MailboxCrypto::Decrypt(const std::string &key, const std::string &nonce, const std::string &aad, const std::string &cipher,
                          const std::string &tag, std::string &plain)
   {
      plain.clear();

      if (key.size() != KeyBytes || nonce.size() != NonceBytes || tag.size() != TagBytes || cipher.size() > 0x7FFFFFF0)
         return false;

      EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
      if (!context)
         return false;

      std::vector<unsigned char> out(cipher.size() + 16, 0);
      int length = 0;
      int total = 0;

      bool ok = EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1;
      ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int) NonceBytes, nullptr) == 1;
      ok = ok && EVP_DecryptInit_ex(context, nullptr, nullptr, (const unsigned char *) key.data(), (const unsigned char *) nonce.data()) == 1;
      if (ok && !aad.empty())
         ok = EVP_DecryptUpdate(context, nullptr, &length, (const unsigned char *) aad.data(), (int) aad.size()) == 1;
      if (ok && !cipher.empty())
      {
         ok = EVP_DecryptUpdate(context, &out[0], &length, (const unsigned char *) cipher.data(), (int) cipher.size()) == 1;
         total = length;
      }
      ok = ok && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, (int) TagBytes, (void *) tag.data()) == 1;
      ok = ok && EVP_DecryptFinal_ex(context, &out[0] + total, &length) == 1;
      total += ok ? length : 0;
      EVP_CIPHER_CTX_free(context);

      if (!ok || (size_t) total != cipher.size())
      {
         OPENSSL_cleanse(&out[0], out.size());
         return false;
      }

      plain.assign((const char *) &out[0], (size_t) total);
      OPENSSL_cleanse(&out[0], out.size());
      return true;
   }

   bool
   MailboxCrypto::SealToPublic(const std::string &recipientPublic, const std::string &purpose, const std::string &plain,
                               std::string &sealed, String &problem)
   {
      sealed.clear();

      if (recipientPublic.size() != KeyBytes)
      {
         problem = _T("the public key to seal to is not an X25519 key");
         return false;
      }

      std::string ephemeralPrivate;
      ForgetOnExit forgetEphemeralPrivate(ephemeralPrivate);
      std::string ephemeralPublic;
      if (!GenerateKeyPair(ephemeralPrivate, ephemeralPublic))
      {
         problem = _T("no ephemeral key could be made");
         return false;
      }

      std::string shared = Agree(ephemeralPrivate, recipientPublic);
      ForgetOnExit forgetShared(shared);
      Forget(ephemeralPrivate);
      if (shared.empty())
      {
         problem = _T("the agreement with the public key failed");
         return false;
      }

      std::string key = SealKey(shared, recipientPublic, ephemeralPublic, "hMailServer sealed " + purpose);
      ForgetOnExit forgetKey(key);
      Forget(shared);

      const std::string nonce = RandomBytes(NonceBytes);
      std::string cipher;
      std::string tag;
      const bool encrypted = key.size() == KeyBytes && nonce.size() == NonceBytes &&
                             Encrypt(key, nonce, SealAad(purpose, recipientPublic, ephemeralPublic), plain, cipher, tag);
      Forget(key);

      if (!encrypted)
      {
         problem = _T("the bytes could not be encrypted");
         return false;
      }

      sealed = std::string(Version) + "$x$" + Base64(ephemeralPublic) + "$" + Base64(nonce) + "$" + Base64(cipher + tag);
      return true;
   }

   bool
   MailboxCrypto::OpenSealed(const std::string &recipientPrivate, const std::string &purpose, const std::string &sealed,
                             std::string &plain, String &problem)
   {
      plain.clear();

      const std::vector<std::string> fields = Fields(sealed);
      std::string ephemeralPublic;
      std::string nonce;
      std::string body;
      if (fields.size() != 5 || fields[0] != Version || fields[1] != "x" ||
          !FromBase64(fields[2], ephemeralPublic) || ephemeralPublic.size() != KeyBytes ||
          !FromBase64(fields[3], nonce) || nonce.size() != NonceBytes ||
          !FromBase64(fields[4], body) || body.size() < TagBytes)
      {
         problem = _T("it is not a sealed copy this server reads");
         return false;
      }

      const std::string recipientPublic = PublicKeyOf(recipientPrivate);
      std::string shared = Agree(recipientPrivate, ephemeralPublic);
      ForgetOnExit forgetShared(shared);
      if (recipientPublic.empty() || shared.empty())
      {
         problem = _T("the agreement with the sealed copy's key failed");
         return false;
      }

      std::string key = SealKey(shared, recipientPublic, ephemeralPublic, "hMailServer sealed " + purpose);
      ForgetOnExit forgetKey(key);
      Forget(shared);

      const std::string cipher = body.substr(0, body.size() - TagBytes);
      const std::string tag = body.substr(body.size() - TagBytes);
      const bool opened = Decrypt(key, nonce, SealAad(purpose, recipientPublic, ephemeralPublic), cipher, tag, plain);
      Forget(key);

      if (!opened)
      {
         problem = _T("the key does not open it, or it has been changed");
         return false;
      }

      return true;
   }

   bool
   MailboxCrypto::WrapWithSecret(const AnsiString &secret, const std::string &purpose, const std::string &plain,
                                 std::string &wrapped, String &problem)
   {
      wrapped.clear();

      if (secret.IsEmpty())
      {
         problem = _T("an empty secret wraps nothing");
         return false;
      }

      const std::string salt = RandomBytes(SaltBytes);
      const std::string nonce = RandomBytes(NonceBytes);
      if (salt.size() != SaltBytes || nonce.size() != NonceBytes)
      {
         problem = _T("no random bytes could be had");
         return false;
      }

      std::string key;
      ForgetOnExit forgetKey(key);
      std::string header;

      if (Argon2Available())
      {
         // The server's own password-hash work, never below the floor.
         int memory = HashCreator::ConfiguredArgon2idMemoryKiB();
         int passes = HashCreator::ConfiguredArgon2idTimeCost();
         if (memory < MinimumMemoryKiB) memory = MinimumMemoryKiB;
         if (memory > MaximumMemoryKiB) memory = MaximumMemoryKiB;
         if (passes < MinimumPasses) passes = MinimumPasses;
         if (passes > MaximumPasses) passes = MaximumPasses;
         const int lanes = 1;

         if (!DeriveArgon2(secret, salt, (uint32_t) memory, (uint32_t) passes, (uint32_t) lanes, key))
         {
            problem = _T("Argon2id could not derive a key");
            return false;
         }

         header = std::string(Version) + "$a2$" + NumberText(memory) + "$" + NumberText(passes) + "$" + NumberText(lanes);
      }
      else
      {
         if (!DerivePbkdf2(secret, salt, Pbkdf2Iterations, key))
         {
            problem = _T("PBKDF2 could not derive a key");
            return false;
         }

         header = std::string(Version) + "$p2$" + NumberText(Pbkdf2Iterations);
      }

      std::string cipher;
      std::string tag;
      const bool encrypted = Encrypt(key, nonce, "hMailServer wrapped " + purpose, plain, cipher, tag);
      Forget(key);

      if (!encrypted)
      {
         problem = _T("the key could not be encrypted");
         return false;
      }

      wrapped = header + "$" + Base64(salt) + "$" + Base64(nonce) + "$" + Base64(cipher + tag);
      return true;
   }

   bool
   MailboxCrypto::UnwrapWithSecret(const AnsiString &secret, const std::string &purpose, const std::string &wrapped,
                                   std::string &plain, String &problem)
   {
      plain.clear();

      const std::vector<std::string> fields = Fields(wrapped);
      if (fields.size() < 2 || fields[0] != Version)
      {
         problem = _T("it is not a wrapped key this server reads");
         return false;
      }

      std::string key;
      ForgetOnExit forgetKey(key);
      size_t next = 0;

      if (fields[1] == "a2" && fields.size() == 8)
      {
         int memory = 0;
         int passes = 0;
         int lanes = 0;
         std::string salt;
         if (!Number(fields[2], MinimumMemoryKiB, MaximumMemoryKiB, memory) || !Number(fields[3], MinimumPasses, MaximumPasses, passes) ||
             !Number(fields[4], 1, MaximumLanes, lanes) || !FromBase64(fields[5], salt) || salt.size() != SaltBytes)
         {
            problem = _T("its derivation's parameters are outside what this server accepts");
            return false;
         }

         if (!Argon2Available())
         {
            problem = _T("it was wrapped with Argon2id, which this build's OpenSSL does not have");
            return false;
         }

         if (!DeriveArgon2(secret, salt, (uint32_t) memory, (uint32_t) passes, (uint32_t) lanes, key))
         {
            problem = _T("Argon2id could not derive a key");
            return false;
         }

         next = 6;
      }
      else if (fields[1] == "p2" && fields.size() == 6)
      {
         int iterations = 0;
         std::string salt;
         if (!Number(fields[2], 100000, 10000000, iterations) || !FromBase64(fields[3], salt) || salt.size() != SaltBytes)
         {
            problem = _T("its derivation's parameters are outside what this server accepts");
            return false;
         }

         if (!DerivePbkdf2(secret, salt, iterations, key))
         {
            problem = _T("PBKDF2 could not derive a key");
            return false;
         }

         next = 4;
      }
      else
      {
         problem = _T("it is not a wrapped key this server reads");
         return false;
      }

      std::string nonce;
      std::string body;
      if (!FromBase64(fields[next], nonce) || nonce.size() != NonceBytes || !FromBase64(fields[next + 1], body) || body.size() < TagBytes)
      {
         Forget(key);
         problem = _T("it is not a wrapped key this server reads");
         return false;
      }

      const std::string cipher = body.substr(0, body.size() - TagBytes);
      const std::string tag = body.substr(body.size() - TagBytes);
      const bool opened = Decrypt(key, nonce, "hMailServer wrapped " + purpose, cipher, tag, plain);
      Forget(key);

      if (!opened)
      {
         problem = _T("the secret does not open it");
         return false;
      }

      return true;
   }

   std::string
   MailboxCrypto::StageRecipient(const std::string &parameters)
   {
      if (parameters.size() != (size_t) StageParameterBytes || (unsigned char) parameters[0] != (unsigned char) StageVersion)
         return std::string();

      return parameters.substr(1, KeyBytes);
   }

   bool
   MailboxCrypto::SealStage(const std::string &recipientPublic, const std::string &context, const std::string &plain,
                            std::string &parameters, std::string &cipher, String &problem)
   {
      parameters.clear();
      cipher.clear();

      if (recipientPublic.size() != KeyBytes)
      {
         problem = _T("the account's public key is not an X25519 key");
         return false;
      }

      std::string ephemeralPrivate;
      ForgetOnExit forgetEphemeralPrivate(ephemeralPrivate);
      std::string ephemeralPublic;
      if (!GenerateKeyPair(ephemeralPrivate, ephemeralPublic))
      {
         problem = _T("no ephemeral key could be made");
         return false;
      }

      std::string shared = Agree(ephemeralPrivate, recipientPublic);
      ForgetOnExit forgetShared(shared);
      Forget(ephemeralPrivate);
      if (shared.empty())
      {
         problem = _T("the agreement with the account's public key failed");
         return false;
      }

      std::string key = Hkdf(shared, ephemeralPublic + recipientPublic, StageInfo, KeyBytes);
      ForgetOnExit forgetKey(key);
      Forget(shared);

      const std::string nonce = RandomBytes(NonceBytes);
      const std::string aad = context + recipientPublic + ephemeralPublic;

      std::string tag;
      bool ok = key.size() == KeyBytes && nonce.size() == NonceBytes && Encrypt(key, nonce, aad, plain, cipher, tag);

      // Opened again before it is answered, with the same key: what is
      // returned is known to open, and a fault in the cipher is caught here
      // rather than at the first read of the stored message.
      if (ok)
      {
         std::string check;
         ForgetOnExit forgetCheck(check);
         ok = Decrypt(key, nonce, aad, cipher, tag, check) && check == plain;
         Forget(check);
      }
      Forget(key);

      if (!ok)
      {
         cipher.clear();
         problem = _T("the message could not be encrypted to the account's key");
         return false;
      }

      parameters.push_back((char) StageVersion);
      parameters += recipientPublic;
      parameters += ephemeralPublic;
      parameters += nonce;
      parameters += tag;
      return true;
   }

   bool
   MailboxCrypto::OpenStage(const std::string &parameters, const std::string &context, const std::string &cipher,
                            const KeyFinder &keys, std::string &plain, String &problem)
   {
      plain.clear();

      if (parameters.size() != (size_t) StageParameterBytes || (unsigned char) parameters[0] != (unsigned char) StageVersion)
      {
         problem = _T("its encryption stage's parameters are not ones this server reads");
         return false;
      }

      const std::string recipientPublic = parameters.substr(1, KeyBytes);
      const std::string ephemeralPublic = parameters.substr(1 + KeyBytes, KeyBytes);
      const std::string nonce = parameters.substr(1 + 2 * KeyBytes, NonceBytes);
      const std::string tag = parameters.substr(1 + 2 * KeyBytes + NonceBytes, TagBytes);

      std::string privateKey;
      ForgetOnExit forgetPrivateKey(privateKey);
      if (!keys || !keys(recipientPublic, privateKey, problem))
      {
         if (problem.IsEmpty())
            problem = _T("no key was given to open it");
         return false;
      }

      std::string shared = Agree(privateKey, ephemeralPublic);
      ForgetOnExit forgetShared(shared);
      Forget(privateKey);
      if (shared.empty())
      {
         problem = _T("the agreement with its ephemeral key failed");
         return false;
      }

      std::string key = Hkdf(shared, ephemeralPublic + recipientPublic, StageInfo, KeyBytes);
      ForgetOnExit forgetKey(key);
      Forget(shared);

      const bool opened = Decrypt(key, nonce, context + recipientPublic + ephemeralPublic, cipher, tag, plain);
      Forget(key);

      if (!opened)
      {
         problem = _T("its encrypted body does not open with the account's key - it has been changed, or it is another key's");
         return false;
      }

      return true;
   }

   void
   MailboxCrypto::Test()
   {
      // RFC 7748 section 6.1: Alice and Bob.
      const std::string alicePrivate = FromHexLiteral("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
      const std::string alicePublic = FromHexLiteral("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
      const std::string bobPrivate = FromHexLiteral("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
      const std::string bobPublic = FromHexLiteral("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
      const std::string shared = FromHexLiteral("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

      Assert::IsTrue(PublicKeyOf(alicePrivate) == alicePublic);
      Assert::IsTrue(PublicKeyOf(bobPrivate) == bobPublic);
      Assert::IsTrue(Agree(alicePrivate, bobPublic) == shared);
      Assert::IsTrue(Agree(bobPrivate, alicePublic) == shared);

      // RFC 5869 appendix A.1, the first 32 bytes of the 42.
      const std::string okm = Hkdf(FromHexLiteral("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"),
                                   FromHexLiteral("000102030405060708090a0b0c"),
                                   FromHexLiteral("f0f1f2f3f4f5f6f7f8f9"), 32);
      Assert::IsTrue(okm == FromHexLiteral("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"));

      // Base64 both ways, padding and all.
      std::string decoded;
      Assert::IsTrue(Base64("Test") == "VGVzdA==");
      Assert::IsTrue(FromBase64("VGVzdA==", decoded) && decoded == "Test");
      Assert::IsTrue(FromBase64(Base64(alicePrivate), decoded) && decoded == alicePrivate);
      Assert::IsFalse(FromBase64("VGV$dA==", decoded));

      // A new pair, and a seal to it: opened by its key, refused by another,
      // refused when a byte changes, refused for another purpose.
      std::string mailboxPrivate;
      std::string mailboxPublic;
      Assert::IsTrue(GenerateKeyPair(mailboxPrivate, mailboxPublic));
      Assert::IsTrue(PublicKeyOf(mailboxPrivate) == mailboxPublic);

      String problem;
      std::string sealed;
      std::string opened;
      Assert::IsTrue(SealToPublic(mailboxPublic, "recovery", "the secret", sealed, problem));
      Assert::IsTrue(OpenSealed(mailboxPrivate, "recovery", sealed, opened, problem) && opened == "the secret");
      Assert::IsFalse(OpenSealed(bobPrivate, "recovery", sealed, opened, problem));
      Assert::IsFalse(OpenSealed(mailboxPrivate, "another purpose", sealed, opened, problem));
      std::string changed = sealed;
      changed[changed.size() - 3] = changed[changed.size() - 3] == 'A' ? 'B' : 'A';
      Assert::IsFalse(OpenSealed(mailboxPrivate, "recovery", changed, opened, problem));

      // A wrap under a secret: the right one opens it, a wrong one and another
      // purpose do not, a changed byte does not, and parameters out of bounds
      // are refused before any work is spent on them.
      std::string wrapped;
      Assert::IsTrue(WrapWithSecret("correct horse", "password", mailboxPrivate, wrapped, problem));
      Assert::IsTrue(UnwrapWithSecret("correct horse", "password", wrapped, opened, problem) && opened == mailboxPrivate);
      Assert::IsFalse(UnwrapWithSecret("wrong horse", "password", wrapped, opened, problem));
      Assert::IsFalse(UnwrapWithSecret("correct horse", "recovery", wrapped, opened, problem));

      // A changed byte of the wrapped key - a character of the last field, the
      // ciphertext and its tag, which has no padding to hide a change in.
      std::string changedWrap = wrapped;
      changedWrap[changedWrap.size() - 3] = changedWrap[changedWrap.size() - 3] == 'A' ? 'B' : 'A';
      Assert::IsFalse(UnwrapWithSecret("correct horse", "password", changedWrap, opened, problem));
      Assert::IsFalse(UnwrapWithSecret("correct horse", "password", "hmk1$a2$8$1$1$AAAAAAAAAAAAAAAAAAAAAA==$AAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAA==", opened, problem));
      Assert::IsFalse(UnwrapWithSecret("correct horse", "password", "not a wrap", opened, problem));

      // A message's stage: opened by the finder's key, refused when the finder
      // has none, when the context differs and when a byte of the body changes.
      const std::string message = "From: a@example.test\r\nSubject: at rest\r\n\r\nThe body.\r\n";
      std::string parameters;
      std::string cipher;
      Assert::IsTrue(SealStage(mailboxPublic, "context", message, parameters, cipher, problem));
      Assert::IsTrue(parameters.size() == (size_t) StageParameterBytes && cipher.size() == message.size() && cipher != message);
      Assert::IsTrue(StageRecipient(parameters) == mailboxPublic);

      const KeyFinder right = [&mailboxPrivate, &mailboxPublic] (const std::string &recipient, std::string &key, String &) {
         if (recipient != mailboxPublic) return false;
         key = mailboxPrivate;
         return true;
      };
      const KeyFinder closed = [] (const std::string &, std::string &, String &why) { why = _T("closed"); return false; };

      Assert::IsTrue(OpenStage(parameters, "context", cipher, right, opened, problem) && opened == message);
      Assert::IsFalse(OpenStage(parameters, "context", cipher, closed, opened, problem));
      Assert::IsFalse(OpenStage(parameters, "another context", cipher, right, opened, problem));
      std::string changedCipher = cipher;
      changedCipher[5] = (char) (changedCipher[5] ^ 0x01);
      Assert::IsFalse(OpenStage(parameters, "context", changedCipher, right, opened, problem));

      Forget(mailboxPrivate);
      Assert::IsTrue(mailboxPrivate.empty());

      TestForgetOnExit();
   }

   namespace
   {
      // Every byte of the first length bytes of the string's buffer is the byte.
      // The buffer is the string's own still - Forget clears it without giving
      // it back - so what is read is what the guard left there.
      bool BufferHolds(const std::string &watched, const char *buffer, size_t length, char byte)
      {
         if (watched.capacity() < length)
            return false;

         for (size_t i = 0; i < length; ++i)
         {
            if (buffer[i] != byte)
               return false;
         }

         return true;
      }

      void ThrowWhileWatched(std::string &secret)
      {
         MailboxCrypto::ForgetOnExit forget(secret);
         throw std::runtime_error("an error between the key's use and the end of its scope");
      }

      bool ReturnWhileWatched(std::string &secret, bool early)
      {
         MailboxCrypto::ForgetOnExit forget(secret);
         if (early)
            return false;

         return secret.size() == 48;
      }

      void LeaveUnwatched(std::string &secret, bool fail)
      {
         if (fail)
            throw std::runtime_error("an error with nothing watching the key");
         (void) secret;
      }

      // StoredSecrets::ImportText's shape: lines read into a vector one by one,
      // and the scope left by an exception - from an allocation, say - before
      // the hand-written Forget of each line was reached.
      void ThrowWhileEachWatched(std::vector<std::string> &lines)
      {
         MailboxCrypto::ForgetEachOnExit forget(lines);
         throw std::runtime_error("an error between reading the lines and forgetting them");
      }
   }

   // MailboxKeys.cpp's private keys are watched by ForgetOnExit: the bytes are
   // written over whether the scope ends by a return, early or late, or by an
   // exception thrown through it - and, the control, a key nothing watches is
   // still there after the same exception, so a probe that sees zeros is seeing
   // the guard's work.
   void
   MailboxCrypto::TestForgetOnExit()
   {
      // Longer than any short-string buffer, so the bytes are on the heap as a
      // key's are.
      const size_t length = 48;

      std::string thrown(length, 'k');
      const char *thrownBuffer = thrown.data();
      bool caught = false;
      try
      {
         ThrowWhileWatched(thrown);
      }
      catch (const std::runtime_error &)
      {
         caught = true;
      }
      Assert::IsTrue(caught && thrown.empty() && BufferHolds(thrown, thrownBuffer, length, 0));

      std::string early(length, 'k');
      const char *earlyBuffer = early.data();
      Assert::IsFalse(ReturnWhileWatched(early, true));
      Assert::IsTrue(early.empty() && BufferHolds(early, earlyBuffer, length, 0));

      std::string late(length, 'k');
      const char *lateBuffer = late.data();
      Assert::IsTrue(ReturnWhileWatched(late, false));
      Assert::IsTrue(late.empty() && BufferHolds(late, lateBuffer, length, 0));

      // The control: the same exception with nothing watching leaves every byte.
      std::string unwatched(length, 'k');
      const char *unwatchedBuffer = unwatched.data();
      caught = false;
      try
      {
         LeaveUnwatched(unwatched, true);
      }
      catch (const std::runtime_error &)
      {
         caught = true;
      }
      Assert::IsTrue(caught && unwatched.size() == length && BufferHolds(unwatched, unwatchedBuffer, length, 'k'));
      Forget(unwatched);

      // Every line of a vector, its scope left by an exception: each written over
      // and emptied, the vector itself left for its owner to free.
      std::vector<std::string> lines;
      lines.push_back(std::string(length, 'a'));
      lines.push_back(std::string(length, 'b'));
      lines.push_back(std::string(length, 'c'));
      std::vector<const char *> lineBuffers;
      for (const std::string &line : lines)
         lineBuffers.push_back(line.data());
      caught = false;
      try
      {
         ThrowWhileEachWatched(lines);
      }
      catch (const std::runtime_error &)
      {
         caught = true;
      }
      Assert::IsTrue(caught && lines.size() == 3);
      for (size_t i = 0; i < lines.size(); ++i)
         Assert::IsTrue(lines[i].empty() && BufferHolds(lines[i], lineBuffers[i], length, 0));
   }
}
