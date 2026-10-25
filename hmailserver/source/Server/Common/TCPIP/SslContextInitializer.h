// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

// The protocol floor every boost::asio::ssl::context in this server is built with,
// spelled out as one constant expression on purpose: the analyser that reads
// set_options (CodeQL's boost::asio TLS-settings check) accepts only a
// compile-time constant carrying no_sslv3, no_tlsv1 and no_tlsv1_1, and refuses a
// mask computed at run time. So the floor is constant and complete - SSLv2, SSLv3,
// TLS 1.0 and TLS 1.1 off - and the [Settings] toggles that let an administrator
// re-enable TLS 1.0 or 1.1 for an old mail client act afterwards, natively, in
// SslContextInitializer::SetContextOptions_, and only for the mail listeners and
// the mail client. The HTTPS clients keep the floor as it is.
//
// The floor also keeps RFC 8879 certificate compression off in both directions
// (HM_TLS_NO_CERTIFICATE_COMPRESSION), so that no context - the two HTTPS clients
// that are set up with the floor alone, the MTA-STS policy fetch (TlsPolicy) and
// the OAuth2 token request (OutboundOAuth2TokenClient), among them - offers to take
// a peer's certificate compressed: that would feed a decompressor from the peer
// before its certificate is checked, and the policy fetch reaches whatever host a
// recipient's domain names. See NoCertificateCompression in SslContextInitializer.cpp
// for why the two options are set by number.
#define HM_TLS_NO_CERTIFICATE_COMPRESSION \
   ((static_cast<uint64_t>(1) << 32) | (static_cast<uint64_t>(1) << 33))

#define HM_TLS_CONTEXT_FLOOR \
   (boost::asio::ssl::context::default_workarounds | \
    boost::asio::ssl::context::single_dh_use | \
    boost::asio::ssl::context::no_sslv2 | \
    boost::asio::ssl::context::no_sslv3 | \
    boost::asio::ssl::context::no_tlsv1 | \
    boost::asio::ssl::context::no_tlsv1_1 | \
    HM_TLS_NO_CERTIFICATE_COMPRESSION)

namespace HM
{
   class SSLCertificate;

   class SslContextInitializer
   {
   public:

      static bool InitServer(boost::asio::ssl::context& context, std::shared_ptr<SSLCertificate> certificate, String ip_address, int port);
      // honour_legacy_toggles: the mail client follows the [Settings] TLS version
      // toggles like the listeners do; an HTTPS client of a web service passes
      // false and keeps TLS 1.2 as its floor whatever the toggles say.
      static bool InitClient(boost::asio::ssl::context& context, bool honour_legacy_toggles = true);


   private:

      friend class SslContextInitializerTester;

      static void SetContextOptions_(boost::asio::ssl::context& context, bool honour_legacy_toggles);

      // What SetContextOptions_ reads from the configuration: the four TLS versions
      // (after its fallback, so at least one is on) and the two cipher-order options.
      struct ContextOptionChoices
      {
         bool tlsv10 = false;
         bool tlsv11 = false;
         bool tlsv12 = true;
         bool tlsv13 = true;
         bool prefer_server_ciphers = false;
         bool prioritize_chacha = false;
      };

      // Sets the options the choices ask for, and those every context has, on a
      // context built with HM_TLS_CONTEXT_FLOOR. Apart from SetContextOptions_ so
      // that SslContextInitializerTester can set up a context exactly as a listener's
      // is set up, without a database, and read back what OpenSSL holds.
      static void ApplyContextOptions_(boost::asio::ssl::context& context, const ContextOptionChoices &choices, bool honour_legacy_toggles);

      // The PEM passphrase callback body. OpenSSL only invokes it when the private
      // key file is actually encrypted, so an unencrypted key never reaches this
      // and behaves exactly as it always has. Returns the passphrase configured on
      // the certificate, or an empty string - which makes the key load fail and be
      // reported by InitServer - when none is configured.
      static std::string GetPassword_(std::shared_ptr<SSLCertificate> certificate);

      // Session resumption policy for a server context: an explicit session-ID
      // context, cache size and lifetime control, the ability to turn session
      // tickets off, and ticket-key rotation. Server-side only - none of these
      // apply to the outbound client context. Every setting defaults to "make no
      // OpenSSL call at all", so a default configuration keeps OpenSSL's stock
      // behaviour; see the function body for which knob defends which failure.
      static void SetSessionResumption_(boost::asio::ssl::context& context, const String &ip_address, int port);

      static void SetCipherList_(boost::asio::ssl::context& context);
      static void SetTls13CipherSuites_(boost::asio::ssl::context& context);

      // Sets the TLS key-exchange group list. Named "groups" rather than "curves"
      // because the list now carries hybrid post-quantum KEMs (X25519MLKEM768 and
      // friends) alongside the classical elliptic curves.
      static void SetKeyExchangeGroups_(boost::asio::ssl::context& context);
      // One report per distinct message: see the definition.
      static void ReportGroupListOnce_(int severity, int code, const String &message);

      // False if the given OpenSSL group list would end up enabling no group at
      // all, which OpenSSL accepts but which makes TLS 1.3 unusable.
      static bool ContainsGroupToAdd_(const AnsiString& groupList);

      // Reports an expired or not-yet-valid certificate on a context that has just
      // loaded one. OpenSSL checks the validity period of certificates it verifies
      // and not of the one it is told to serve, so without this an expired
      // certificate is served silently and fails every handshake at the client.
      // Reports; does not fail the listener - see the call site.
      static void ReportCertificateValidityProblem_(boost::asio::ssl::context& context, const String &certificate_file, const String &ip_address, int port);

      // First error on OpenSSL's per-thread error queue, as text. Empties the
      // queue so our failure is not later reported against an unrelated call.
      static AnsiString GetOpenSslError_();

   };

   // The options every TLS context of the server holds once it is set up, read back
   // with SSL_CTX_get_options: each combination of the four TLS versions and the two
   // cipher-order settings, for a listener's context and for the mail client's and an
   // HTTPS client's, holds exactly the options SetContextOptions_ asks for and none
   // from bit 32 up that it does not ask for. Until 6.3.6 the mask was held in an
   // int; SSL_OP_ALL carries bit 31, so the int was negative and its conversion to
   // SSL_CTX_set_options' uint64_t set every option from bit 32 up. Throws
   // std::logic_error naming the first thing that is wrong. A context set up with
   // HM_TLS_CONTEXT_FLOOR alone, as the MTA-STS policy fetch and the OAuth2 token
   // request set theirs up, keeps certificate compression off as the others do.
   // Where the server's configuration is loaded (inside the service, through
   // ClassTester), the contexts InitClient sets up from it are read back as well.
   class SslContextInitializerTester
   {
   public:
      void Test();

   private:
      void TestChoices_(const SslContextInitializer::ContextOptionChoices &choices, bool honour_legacy_toggles, bool listener);
      void TestFloor_();
      void TestInitClient_();
   };
}