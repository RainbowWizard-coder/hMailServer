// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// DANE (RFC 7672) certificate verification for outbound SMTP connections.

#pragma once

#include <string>
#include <vector>

namespace HM
{
   // One TLSA record (RFC 6698). The data member contains the raw
   // certificate association data from the record.
   struct TlsaRecord
   {
      int usage = 0;          // 0=PKIX-TA, 1=PKIX-EE, 2=DANE-TA, 3=DANE-EE
      int selector = 0;       // 0=full certificate, 1=SubjectPublicKeyInfo
      int matching_type = 0;  // 0=exact, 1=SHA-256, 2=SHA-512
      std::string data;
   };

   // DANE (RFC 7672) for an outbound SMTP connection, done by OpenSSL's own
   // DANE support (SSL_dane_enable, RFC 7671): a DANE-EE(3) record matches the
   // server's certificate itself, with no chain, name or date checked - the
   // record is the trust anchor (RFC 7672 section 3.1.1); a DANE-TA(2) record
   // matches a certificate of the chain the server sends, or holds the trust
   // anchor's whole certificate or key, and the server's certificate must then
   // chain to it, be in date and name the MX host (sections 3.1.2 and 3.2).
   // PKIX-TA(0) and PKIX-EE(1) are unusable in SMTP (section 3.1.3).
   //
   // Until 6.3.6 only DANE-EE was matched, by a callback of this class's own,
   // and a domain that published DANE-TA records alone was delivered to as if
   // it published none - without TLS, if the STARTTLS offer was taken away.
   class DaneVerifier
   {
   public:
      explicit DaneVerifier(int session_id);

      // Whether a record is one this server can authenticate a server with:
      // DANE-TA(2) or DANE-EE(3), selector 0 or 1, matching type 0, 1 or 2,
      // and data OpenSSL takes for them - a digest of the digest's length, or
      // a certificate or key that parses. A secure RRset with none of these is
      // "unusable" in RFC 7672's sense (section 2.2): TLS is still required.
      static bool IsUsable(const TlsaRecord &record);

      // Arms OpenSSL's DANE for one connection before its handshake, with
      // host_name (the MX host) as the TLSA base domain and the name a
      // DANE-TA chain must carry. The number of records OpenSSL took, which is
      // 0 when none could be added or DANE could not be enabled at all - and
      // the handshake must then fail, never go on unauthenticated.
      static int Arm(SSL *ssl, const AnsiString &host_name, const std::vector<TlsaRecord> &records, int session_id);

      // The handshake's verify callback once Arm has run: OpenSSL has done the
      // DANE matching, and preverified is its verdict at each depth. Logged at
      // the end-entity certificate.
      bool operator() (bool preverified, boost::asio::ssl::verify_context& ctx) const;

   private:

      int session_id_;
   };
}
