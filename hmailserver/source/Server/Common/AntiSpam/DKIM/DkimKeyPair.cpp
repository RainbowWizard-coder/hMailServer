// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "DkimKeyPair.h"
#include "../../Util/Encoding/Base64.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   bool
   DkimKeyPair::Generate(int bits, AnsiString &pem, String &error)
   {
      pem = "";

      if (bits != 2048 && bits != 3072 && bits != 4096)
      {
         error = Formatter::Format(_T("A DKIM key of {0} bits is not generated; the sizes are 2048, 3072 and 4096."), bits);
         return false;
      }

      EVP_PKEY *key = EVP_RSA_gen((unsigned int) bits);
      if (key == nullptr)
      {
         ERR_clear_error();
         error = Formatter::Format(_T("OpenSSL could not generate a {0}-bit RSA key."), bits);
         return false;
      }

      BIO *memory = BIO_new(BIO_s_mem());
      if (memory == nullptr)
      {
         EVP_PKEY_free(key);
         error = _T("OpenSSL could not allocate a buffer for the key.");
         return false;
      }

      // PKCS#8, "BEGIN PRIVATE KEY": what PEM_read_bio_PrivateKey reads back in
      // DKIM::Sign, and what the Control Panel's generator has always written.
      bool written = PEM_write_bio_PrivateKey(memory, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
      EVP_PKEY_free(key);

      if (written)
      {
         char *data = nullptr;
         long length = BIO_get_mem_data(memory, &data);
         if (length > 0 && data != nullptr)
            pem = AnsiString(data, (int) length);
      }

      BIO_free(memory);

      if (pem.IsEmpty())
      {
         ERR_clear_error();
         error = _T("OpenSSL could not write the generated key as PEM.");
         return false;
      }

      return true;
   }

   bool
   DkimKeyPair::Derive(const AnsiString &pem, AnsiString &keyType, std::vector<unsigned char> &publicKey)
   {
      keyType = "";
      publicKey.clear();

      if (pem.IsEmpty())
         return false;

      BIO *bio = BIO_new_mem_buf(pem.c_str(), pem.GetLength());
      if (bio == nullptr)
         return false;

      EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);

      if (key == nullptr)
      {
         ERR_clear_error();
         return false;
      }

      bool ok = false;

      if (EVP_PKEY_base_id(key) == EVP_PKEY_ED25519)
      {
         // RFC 8463 section 4.2: p= is the base64 of the raw 32-byte public key,
         // not a SubjectPublicKeyInfo.
         size_t length = 0;
         if (EVP_PKEY_get_raw_public_key(key, nullptr, &length) == 1 && length > 0)
         {
            publicKey.resize(length);
            if (EVP_PKEY_get_raw_public_key(key, &publicKey[0], &length) == 1)
            {
               publicKey.resize(length);
               keyType = "ed25519";
               ok = true;
            }
         }
      }
      else if (EVP_PKEY_base_id(key) == EVP_PKEY_RSA)
      {
         int length = i2d_PUBKEY(key, nullptr);
         if (length > 0)
         {
            publicKey.resize((size_t) length);
            unsigned char *cursor = &publicKey[0];
            if (i2d_PUBKEY(key, &cursor) == length)
            {
               keyType = "rsa";
               ok = true;
            }
         }
      }

      EVP_PKEY_free(key);
      ERR_clear_error();

      if (!ok)
         publicKey.clear();

      return ok;
   }

   bool
   DkimKeyPair::DeriveFromFile(const String &path, AnsiString &keyType, std::vector<unsigned char> &publicKey, String &error)
   {
      if (path.IsEmpty())
      {
         error = _T("No private key file is configured.");
         return false;
      }

      if (!FileUtilities::Exists(path))
      {
         error = Formatter::Format(_T("The private key file {0} does not exist."), path);
         return false;
      }

      AnsiString pem = FileUtilities::ReadCompleteTextFile(path);
      if (pem.IsEmpty())
      {
         error = Formatter::Format(_T("The private key file {0} could not be read."), path);
         return false;
      }

      if (!Derive(pem, keyType, publicKey))
      {
         error = Formatter::Format(_T("The private key file {0} does not hold an RSA or Ed25519 private key in PEM."), path);
         return false;
      }

      return true;
   }

   AnsiString
   DkimKeyPair::RecordValue(const AnsiString &keyType, const std::vector<unsigned char> &publicKey)
   {
      if (publicKey.empty())
         return "";

      AnsiString encoded = Base64::Encode((const char *) &publicKey[0], (int) publicKey.size());
      return "v=DKIM1; k=" + keyType + "; p=" + encoded;
   }
}
