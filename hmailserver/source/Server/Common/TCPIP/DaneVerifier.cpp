// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// DANE (RFC 7672) certificate verification for outbound SMTP connections.

#include "StdAfx.h"

#include "DaneVerifier.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   DaneVerifier::DaneVerifier(int session_id) :
      session_id_(session_id)
   {

   }

   bool
   DaneVerifier::IsUsable(const TlsaRecord &record)
   {
      // RFC 7672 section 3.1.3: PKIX-TA(0) and PKIX-EE(1) are not for SMTP.
      if (record.usage != 2 && record.usage != 3)
         return false;
      if (record.selector != 0 && record.selector != 1)
         return false;
      if (record.data.empty() || record.data.size() > 65535)
         return false;

      switch (record.matching_type)
      {
      case 1:
         return record.data.size() == 32;
      case 2:
         return record.data.size() == 64;
      case 0:
         {
            // Full(0): the certificate or the key itself, which OpenSSL parses
            // when the record is added and refuses when it does not parse, or
            // when anything follows it.
            const unsigned char *start = reinterpret_cast<const unsigned char *>(record.data.data());
            const unsigned char *cursor = start;
            const long length = static_cast<long>(record.data.size());
            bool parsed = false;

            if (record.selector == 0)
            {
               X509 *certificate = d2i_X509(nullptr, &cursor, length);
               parsed = certificate != nullptr && cursor == start + length && X509_get0_pubkey(certificate) != nullptr;
               X509_free(certificate);
            }
            else
            {
               EVP_PKEY *key = d2i_PUBKEY(nullptr, &cursor, length);
               parsed = key != nullptr && cursor == start + length;
               EVP_PKEY_free(key);
            }

            // A record that does not parse leaves its reason in this thread's
            // OpenSSL error queue, where the next TLS failure would be blamed on it.
            ERR_clear_error();
            return parsed;
         }
      default:
         return false;
      }
   }

   int
   DaneVerifier::Arm(SSL *ssl, const AnsiString &host_name, const std::vector<TlsaRecord> &records, int session_id)
   {
      if (ssl == nullptr || host_name.IsEmpty())
         return 0;

      // The TLSA base domain, which for a DANE-TA match is also the name the
      // server's certificate must carry (RFC 7672 section 3.2.2), and the SNI
      // name when none is set yet.
      if (SSL_dane_enable(ssl, host_name.c_str()) <= 0)
      {
         ERR_clear_error();

         String message;
         message.Format(_T("DANE: Session %d: OpenSSL's DANE could not be enabled for this connection."), session_id);
         LOG_TCPIP(message);
         return 0;
      }

      // A DANE-EE match authenticates the server whatever its certificate names
      // (RFC 7672 section 3.1.1), as OpenSSL's documentation says an SMTP client
      // should ask for; and a wildcard in a DANE-TA chain's name stands for one
      // whole left-most label only (section 3.2.3).
      SSL_dane_set_flags(ssl, DANE_FLAG_NO_DANE_EE_NAMECHECKS);
      SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

      int armed = 0;
      for (const TlsaRecord &record : records)
      {
         if (!IsUsable(record))
            continue;

         const int added = SSL_dane_tlsa_add(ssl, static_cast<uint8_t>(record.usage), static_cast<uint8_t>(record.selector),
            static_cast<uint8_t>(record.matching_type), reinterpret_cast<const unsigned char *>(record.data.data()), record.data.size());

         if (added > 0)
            armed++;
      }

      ERR_clear_error();
      return armed;
   }

   bool
   DaneVerifier::operator() (bool preverified, boost::asio::ssl::verify_context& ctx) const
   {
      X509_STORE_CTX *store = ctx.native_handle();
      const int depth = X509_STORE_CTX_get_error_depth(store);

      if (!preverified)
      {
         // No record matched (X509_V_ERR_DANE_NO_MATCH), or a DANE-TA chain is
         // broken, out of date or for another name. OpenSSL's reason, so that an
         // administrator can tell which.
         const int error = X509_STORE_CTX_get_error(store);

         String message;
         message.Format(_T("DANE: Session %d: The certificate at depth %d did not authenticate against the TLSA records: OpenSSL error %d, %s. Failing TLS handshake."),
            session_id_, depth, error, String(X509_verify_cert_error_string(error)).c_str());
         LOG_TCPIP(message);
         return false;
      }

      if (depth == 0)
      {
         // The end-entity certificate is the last one OpenSSL reports, and by
         // then the record that authenticated the chain is known.
         SSL *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));

         uint8_t usage = 0;
         uint8_t selector = 0;
         uint8_t matchingType = 0;
         const int matchDepth = ssl != nullptr ? SSL_get0_dane_tlsa(ssl, &usage, &selector, &matchingType, nullptr, nullptr) : -1;

         String message;
         if (matchDepth >= 0)
         {
            message.Format(_T("DANE: Session %d: Certificate authenticated by a %s TLSA record (usage %d, selector %d, matching type %d) at depth %d."),
               session_id_, usage == 2 ? _T("DANE-TA") : _T("DANE-EE"), (int) usage, (int) selector, (int) matchingType, matchDepth);
         }
         else
         {
            message.Format(_T("DANE: Session %d: Certificate authenticated against the TLSA records."), session_id_);
         }
         LOG_TCPIP(message);
      }

      return true;
   }
}
