// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2005 Martin Knafve / hMailServer.com.

// Created 2005-07-21

#pragma once

#include "SocketConstants.h"

namespace HM
{
   class CertificateVerifier
   {
   public:

      /// The type of the function object's result.
      typedef bool result_type;

      /// Constructor. forgive_failure: a certificate that does not verify is
      /// accepted, as on an opportunistic connection, although TLS itself is
      /// required - RFC 7672 section 2.2's DNSSEC-validated TLSA records of
      /// which none is usable, where TLS is required and authentication not.
      CertificateVerifier(int session_id, ConnectionSecurity connection_security,  const String &host_name, bool forgive_failure = false);

      /// Perform certificate verification.
      bool operator()(bool preverified, boost::asio::ssl::verify_context& ctx) const;

      // Whether a certificate is for the host a connection was made to, the way
      // the POSIX branch decides it: an address - an endpoint named by its IP,
      // 192.0.2.10 or 2001:db8::10, in a URL's brackets or not - by the
      // certificate's iPAddress entries (X509_check_ip_asc), and otherwise by
      // X509_check_host as before - for an address, one the certificate writes as
      // a dNSName, never through a wildcard, or as its common name when it has no
      // iPAddress entry. The Windows branch asks CryptoAPI, whose SSL policy reads
      // iPAddress entries itself (measured on 25 September 2026).
      static bool MatchesHost(X509 *certificate, const AnsiString &host);

   private:

      // Whether the certificate's subjectAltName has an iPAddress entry.
      static bool NamesAnAddress_(X509 *certificate);

      bool VerifyCertificate_( PCCERT_CONTEXT certificate, LPWSTR serverName, int &windows_error_code) const;

      bool OverrideResult_(bool result) const;

      // The host name to be checked.
      ConnectionSecurity connection_security_;
      String host_name_;
      int session_id_;
      bool forgive_failure_;
   };

   // Verifies the certificate an INBOUND session presents when a port's
   // ClientCertificatePolicy asks for one.
   //
   // A separate class rather than a mode on CertificateVerifier, because every
   // assumption CertificateVerifier makes is a server-certificate assumption:
   // it builds the chain with Windows' AUTHTYPE_SERVER policy, it matches an
   // expected host name (a client certificate has none), and OverrideResult_
   // deliberately forgives any failure on CSSTARTTLSOptional connections -
   // correct for opportunistic OUTBOUND TLS (RFC 7435), and precisely the
   // forgiveness that must never apply when an administrator has said a port
   // REQUIRES a trusted client certificate. Keeping the inbound path in its own
   // class means no future change to either can quietly weaken the other.
   //
   // The chain itself is validated by OpenSSL's built-in verification against
   // the CA bundle TCPServer loaded into the port's SSL context - that is what
   // 'preverified' reports - so this callback only has to decide what a failure
   // means for the session, and to name the certificate in the log so the
   // administrator can tell WHICH client was refused.
   class ClientCertificateVerifier
   {
   public:

      /// The type of the function object's result.
      typedef bool result_type;

      // fail_handshake_on_error: true for CCPRequire (an unverifiable
      // certificate ends the handshake), false for CCPRequest (the outcome is
      // logged and the session continues; that mode exists for inventorying
      // clients before enforcement is switched on).
      ClientCertificateVerifier(int session_id, bool fail_handshake_on_error);

      /// Perform certificate verification.
      bool operator()(bool preverified, boost::asio::ssl::verify_context& ctx) const;

   private:

      int session_id_;
      bool fail_handshake_on_error_;
   };

   // CertificateVerifier::MatchesHost against certificates made for the purpose:
   // an address as an iPAddress entry only (IPv4 and IPv6), an address as a
   // dNSName, an address as the common name, and a name.
   class CertificateVerifierTester
   {
   public:
      void Test();

   private:
      static bool Matches_(const char *pem, const char *host);
   };
}