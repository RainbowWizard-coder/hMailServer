// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "CertificateVerifier.h"
#include "SocketConstants.h"

#include <openssl/x509.h>

// X509_check_host, X509_check_ip_asc and X509_check_purpose live here rather than
// in <openssl/x509.h>. They are the questions CryptoAPI's CERT_CHAIN_POLICY_SSL asks
// over and above chain trust, and the POSIX branch below asks them by hand;
// MatchesHost, which asks the first two, is compiled on both platforms so that its
// tester runs in the Windows suite too.
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "../Util/Assert.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   CertificateVerifier::CertificateVerifier(int session_id, ConnectionSecurity connection_security, const String &host_name, bool forgive_failure) :
      session_id_(session_id),
      connection_security_(connection_security),
      host_name_(host_name),
      forgive_failure_(forgive_failure)
   {

   }

#ifndef HM_PLATFORM_POSIX
   bool 
   CertificateVerifier::VerifyCertificate_( PCCERT_CONTEXT certificate, LPWSTR server_name,int &windows_error_code) const
   {
      windows_error_code = 0;

      LPSTR usage_identifier[] = { szOID_PKIX_KP_SERVER_AUTH, szOID_SERVER_GATED_CRYPTO, szOID_SGC_NETSCAPE };

      CERT_CHAIN_PARA params = { sizeof( params ) };
      params.RequestedUsage.dwType = USAGE_MATCH_TYPE_OR;
      params.RequestedUsage.Usage.cUsageIdentifier = _countof( usage_identifier );
      params.RequestedUsage.Usage.rgpszUsageIdentifier = usage_identifier;

      PCCERT_CHAIN_CONTEXT chain_context = 0;

      if (!CertGetCertificateChain(NULL, 
                                   certificate,  
                                   NULL,
                                   NULL,
                                   &params,
                                   CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT,
                                   NULL,
                                   &chain_context))
      {
         windows_error_code = GetLastError();
         return false;
      }

      SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslPolicy = { sizeof( sslPolicy ) };
      sslPolicy.dwAuthType = AUTHTYPE_SERVER;
      sslPolicy.pwszServerName = server_name;

      CERT_CHAIN_POLICY_PARA policy = { sizeof( policy ) };
      policy.pvExtraPolicyPara = &sslPolicy;

      CERT_CHAIN_POLICY_STATUS status = { sizeof( status ) };

      BOOL policy_checked = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,
                                                             chain_context,
                                                             &policy,
                                                             &status );

      CertFreeCertificateChain( chain_context );

      windows_error_code = status.dwError;
      bool certificate_ok = policy_checked && status.dwError == 0;
      return certificate_ok;
   }
#endif

   bool CertificateVerifier::operator() (bool preverified, boost::asio::ssl::verify_context& ctx) const
   {
#ifdef HM_PLATFORM_POSIX
      // There is no certificate store to hand a DER blob to here, so the same
      // three questions are asked of OpenSSL, which already has the answers.
      //
      // The Windows branch below ignores 'preverified' entirely and ignores every
      // element of the chain but the leaf, because it hands the leaf to CryptoAPI
      // and CryptoAPI does the whole job - builds the chain, checks trust,
      // revocation, the extended key usage and the host name. Here those are
      // separate mechanisms and each has to be consulted:
      //
      //   * Chain trust is OpenSSL's own, already done by the time this callback
      //     runs, once per chain element. 'preverified' is that verdict, so it is
      //     carried rather than discarded - discarding it for elements above the
      //     leaf, the way the Windows branch does, would accept any chain at all.
      //
      //   * The host name is MatchesHost: X509_check_host, OpenSSL's RFC 6125
      //     matcher - the subjectAltName dNSName entries, falling back to the
      //     common name - and, when the host is an address, X509_check_ip_asc
      //     over the iPAddress entries, which X509_check_host never reads. It is
      //     the same question CERT_CHAIN_POLICY_SSL asks through pwszServerName,
      //     which reads both.
      //
      //   * The extended key usage is X509_check_purpose, which is the
      //     szOID_PKIX_KP_SERVER_AUTH in the usage list the Windows branch builds.
      //     A certificate that is trusted and is for this name but is not allowed
      //     to be a TLS server is still not one to talk to.
      //
      // The failure branch is the same policy either way: OverrideResult_ forgives
      // a failure on an opportunistic connection (RFC 7435) and on nothing else.
      // A refusal is logged with OpenSSL's own reason exactly as the Windows
      // branch logs the Windows one, so an administrator can tell an expired
      // certificate from a wrong name from an untrusted issuer.
      X509_STORE_CTX *store_context = ctx.native_handle();

      const int depth = X509_STORE_CTX_get_error_depth(store_context);

      if (!preverified)
      {
         const int verify_error = X509_STORE_CTX_get_error(store_context);

         LOG_DEBUG(Formatter::Format("Certificate verification failed for session {0}. Expected host: {1}, chain depth: {2}, OpenSSL error {3}: {4}",
            session_id_, host_name_, depth, verify_error, String(X509_verify_cert_error_string(verify_error))));

         return OverrideResult_(false);
      }

      if (depth > 0)
      {
         // A trusted issuer. Nothing above the leaf carries a host name or a TLS
         // server usage to check.
         return OverrideResult_(true);
      }

      X509 *cert = X509_STORE_CTX_get_current_cert(store_context);

      if (cert == nullptr)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5512, "CertificateVerifier::operator()",
            "The certificate verification callback was reached at the end of the chain with no certificate to check.");

         return OverrideResult_(false);
      }

      const AnsiString expected_host_name = host_name_;

      if (!MatchesHost(cert, expected_host_name))
      {
         LOG_DEBUG(Formatter::Format("Certificate verification failed for session {0}. The certificate is not valid for the expected host {1}.",
            session_id_, host_name_));

         return OverrideResult_(false);
      }

      if (::X509_check_purpose(cert, X509_PURPOSE_SSL_SERVER, 0) != 1)
      {
         LOG_DEBUG(Formatter::Format("Certificate verification failed for session {0}. The certificate for host {1} is not usable for TLS server authentication.",
            session_id_, host_name_));

         return OverrideResult_(false);
      }

      LOG_DEBUG(Formatter::Format("Certificate verification succeeded for session {0}.", session_id_));

      return OverrideResult_(true);
#else
      // We're only interested in checking the certificate at the end of the chain.
      int depth = X509_STORE_CTX_get_error_depth(ctx.native_handle());
      if (depth > 0)
         return OverrideResult_(true);

      // Read the cert and convert it to a raw DER-format which we can hand off to Windows.
      X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
      BIO* bio = BIO_new(BIO_s_mem());

      // Convert the certificate from the internal structure to a DER structure in memory ('bio').
      if (i2d_X509_bio(bio,cert) != 1) 
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5512, "CertificateVerifier::operator()", "Failed to convert OpenSSL internal X509 to DER-format.");
         BIO_free(bio);
         return OverrideResult_(false);
      }

      // Read the cert from the BIO structure in memory to a char array.
      int raw_size = BIO_pending(bio);
      unsigned char *raw_certificate = new unsigned char[raw_size];

      int actual_read = BIO_read(bio, raw_certificate, raw_size);

      if (raw_size != actual_read) 
      {
         String errorMessage = Formatter::Format(_T("BIO_read returned an unexpected number of characters. Expected: {0}, Returned: {1}"), raw_size, actual_read);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5513, "CertificateVerifier::operator()", errorMessage);
         BIO_free(bio);
         delete[] raw_certificate;
         return OverrideResult_(false);
      }
      
      // Create a Windows certificate context, using the raw DER data.
      PCCERT_CONTEXT context = CertCreateCertificateContext(X509_ASN_ENCODING, (BYTE*) raw_certificate, raw_size);

      if (context == NULL) 
      {
         String errorMessage = Formatter::Format(_T("Call to CertCreateCertificateContext failed. Error: {0}"), (int) GetLastError());
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5513, "CertificateVerifier::operator()", errorMessage);
         BIO_free(bio);
         delete[] raw_certificate;
         return OverrideResult_(false);
      }

      String expected_host_name = host_name_;

      int windows_error_code = 0;
      if (VerifyCertificate_(context, expected_host_name.GetBuffer(-1), windows_error_code))
      {
         LOG_DEBUG(Formatter::Format("Certificate verification succeeded for session {0}.", session_id_));
         
         BIO_free(bio);
         delete[] raw_certificate;
         CertFreeCertificateContext(context);

         return OverrideResult_(true);
      }
      else
      {
         String windows_error_text = ErrorManager::Instance()->GetWindowsErrorText(windows_error_code);
         String formattedDebugMessage = Formatter::Format("Certificate verification failed for session {0}. Expected host: {1}, Windows error code: {2}, Windows error message: {3}", 
            session_id_, host_name_, windows_error_code, windows_error_text);

         LOG_DEBUG(formattedDebugMessage);

         BIO_free(bio);
         delete[] raw_certificate;
         CertFreeCertificateContext(context);

         return OverrideResult_(false);
      }
#endif

   }

   bool
   CertificateVerifier::MatchesHost(X509 *certificate, const AnsiString &host)
   {
      if (certificate == nullptr || host.IsEmpty())
         return false;

      // An endpoint given by its address - an object store, a webhook, an identity
      // provider or an update feed at https://192.0.2.10/ - presents a certificate
      // that names the address as an iPAddress entry, which is where RFC 5280 puts
      // it and the only place a public CA may; X509_check_host reads dNSName
      // entries and the common name and never that one, so until 25 September 2026
      // such an endpoint was refused on Linux and accepted on Windows. A URL writes
      // an IPv6 address in brackets, which are not part of the address.
      std::string address(host.c_str());
      if (address.size() > 2 && address.front() == '[' && address.back() == ']')
         address = address.substr(1, address.size() - 2);

      boost::system::error_code notAddress;
      boost::asio::ip::make_address(address, notAddress);
      if (notAddress)
         return ::X509_check_host(certificate, host.c_str(), (size_t) host.GetLength(), 0, nullptr) == 1;

      if (::X509_check_ip_asc(certificate, address.c_str(), 0) == 1)
         return true;

      // An address the certificate writes as a dNSName, or as the common name of a
      // certificate with no subjectAltName, was accepted before and CryptoAPI
      // accepts it too, so a certificate made that way keeps working. But never
      // through a wildcard, which names hosts and not addresses (X509_check_host
      // took *.1.2.3 for 10.1.2.3), and not through the common name of a
      // certificate that names addresses as addresses: it has said which addresses
      // it is for, and X509_check_host reads the common name whenever there is no
      // dNSName entry. Both measured on OpenSSL 4.0.2 on 25 September 2026.
      unsigned int flags = X509_CHECK_FLAG_NO_WILDCARDS;
      if (NamesAnAddress_(certificate))
         flags |= X509_CHECK_FLAG_NEVER_CHECK_SUBJECT;

      return ::X509_check_host(certificate, host.c_str(), (size_t) host.GetLength(), flags, nullptr) == 1;
   }

   bool
   CertificateVerifier::NamesAnAddress_(X509 *certificate)
   {
      GENERAL_NAMES *names = (GENERAL_NAMES *) X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr);
      if (names == nullptr)
         return false;

      bool found = false;
      const int count = sk_GENERAL_NAME_num(names);
      for (int i = 0; i < count && !found; i++)
      {
         const GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
         found = name != nullptr && name->type == GEN_IPADD;
      }

      GENERAL_NAMES_free(names);
      return found;
   }

   bool
   CertificateVerifier::OverrideResult_(bool result) const
   {
      if (result == false)
      {
         if (connection_security_ == CSSTARTTLSOptional || forgive_failure_)
            return true;
         else
            return false;
      }

      return true;
   }

   ClientCertificateVerifier::ClientCertificateVerifier(int session_id, bool fail_handshake_on_error) :
      session_id_(session_id),
      fail_handshake_on_error_(fail_handshake_on_error)
   {

   }

   bool
   ClientCertificateVerifier::operator() (bool preverified, boost::asio::ssl::verify_context& ctx) const
   {
      // OpenSSL has already checked this chain element against the CA bundle the
      // port was configured with (TCPServer loads it into the SSL context), so a
      // true preverification is the whole answer. There is deliberately no
      // Windows-store fallback here: the point of a per-port CA bundle is that
      // ONLY the named CA vouches for clients on this port, and consulting the
      // machine's general trust store would let a certificate from any public CA
      // through a port meant for one private one.
      if (preverified)
         return true;

      // Name the failing certificate and the reason. Without this line the
      // administrator of a "require" port sees only a generic handshake failure
      // and cannot tell a partner with an expired certificate from a stranger.
      int depth = X509_STORE_CTX_get_error_depth(ctx.native_handle());
      int verify_error = X509_STORE_CTX_get_error(ctx.native_handle());

      char subject[256] = "(no certificate)";
      X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
      if (cert != nullptr)
         X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));

      String message;
      message.Format(_T("Client certificate verification failed for session %d. Subject: %s, Depth: %d, Error: %s"),
         session_id_, String(subject).c_str(), depth, String(X509_verify_cert_error_string(verify_error)).c_str());
      LOG_TCPIP(message);

      // CCPRequire fails the handshake; CCPRequest records the failure above and
      // lets the session continue - TCPConnection::AsyncHandshakeCompleted reads
      // the surviving error out of SSL_get_verify_result and logs the summary, so
      // an overridden failure is never mistaken for a verified identity.
      return !fail_handshake_on_error_;
   }

   bool
   CertificateVerifierTester::Matches_(const char *pem, const char *host)
   {
      BIO *bio = BIO_new_mem_buf(pem, -1);
      Assert::IsTrue(bio != nullptr);

      X509 *certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
      BIO_free(bio);
      Assert::IsTrue(certificate != nullptr);

      const bool matches = CertificateVerifier::MatchesHost(certificate, AnsiString(host));
      X509_free(certificate);
      return matches;
   }

   void
   CertificateVerifierTester::Test()
   {
      // Self-signed P-256 certificates made with openssl req on 25 September 2026
      // for this test and valid for a hundred years; only their names are read.
      const char *ipOnly =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBpzCCAU6gAwIBAgIUPGpry8oRRukPKR4feEiGl3ZD35EwCgYIKoZIzj0EAwIw\n"
         "FzEVMBMGA1UEAwwMSVAgb25seSB0ZXN0MCAXDTI2MDkyNTEzNDA0OVoYDzIxMjYw\n"
         "OTAxMTM0MDQ5WjAXMRUwEwYDVQQDDAxJUCBvbmx5IHRlc3QwWTATBgcqhkjOPQIB\n"
         "BggqhkjOPQMBBwNCAATmmkBI1DRRoN2Gtdw+7fbrq9ilbSOlqDKgUFtOGGaf+O+m\n"
         "FC5RGS8Fx0E/JXL0oCBq2MZyOjwelXT9Q3qKhzXRo3YwdDAdBgNVHQ4EFgQUKpAV\n"
         "GJbctTIDICE2Z7fRwrLAQH8wHwYDVR0jBBgwFoAUKpAVGJbctTIDICE2Z7fRwrLA\n"
         "QH8wDAYDVR0TAQH/BAIwADATBgNVHSUEDDAKBggrBgEFBQcDATAPBgNVHREECDAG\n"
         "hwQKAQIDMAoGCCqGSM49BAMCA0cAMEQCIAyPgw5KtnzHE5KuO4MFH3LWDtJpaHDP\n"
         "ht3RSDk0FeDhAiBpU8bhdjxRqTnAb3TZ6B9zjRJ2+1xDziGAfTyJMoffiA==\n"
         "-----END CERTIFICATE-----\n";

      const char *ipv6Only =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBujCCAWCgAwIBAgIUFuI98lYZkMj3MM5guppW2uz02/swCgYIKoZIzj0EAwIw\n"
         "GTEXMBUGA1UEAwwOSVB2NiBvbmx5IHRlc3QwIBcNMjYwOTI1MTM0MDQ5WhgPMjEy\n"
         "NjA5MDExMzQwNDlaMBkxFzAVBgNVBAMMDklQdjYgb25seSB0ZXN0MFkwEwYHKoZI\n"
         "zj0CAQYIKoZIzj0DAQcDQgAEwPBnnVGKfoGRQdKunWNJq3IsaMVZWY9FPttUqjqE\n"
         "B+Dcadfp+njJ7KP4mpfzVZcmYq7rO0CZTJQ9lIy9gJk+56OBgzCBgDAdBgNVHQ4E\n"
         "FgQUeegIpQjx1zwr6k5i8UEPfbcsJSowHwYDVR0jBBgwFoAUeegIpQjx1zwr6k5i\n"
         "8UEPfbcsJSowDAYDVR0TAQH/BAIwADATBgNVHSUEDDAKBggrBgEFBQcDATAbBgNV\n"
         "HREEFDAShxAgAQ24AAAAAAAAAAAAAAAHMAoGCCqGSM49BAMCA0gAMEUCIFm9Q87c\n"
         "EYvPTspBERnu1px2H11lRffqq26v77QjXmM0AiEAvdV2l+IYh7zu+X1Vl/NzykGd\n"
         "Fciecvv5+JC/tgdLW4A=\n"
         "-----END CERTIFICATE-----\n";

      const char *ipAsDnsName =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBrDCCAVKgAwIBAgIUHIPAIq7QmEhYro9VBvVBlQlpX+QwCgYIKoZIzj0EAwIw\n"
         "FzEVMBMGA1UEAwwMSVAgYXMgYSBuYW1lMCAXDTI2MDkyNTEzNDA0OVoYDzIxMjYw\n"
         "OTAxMTM0MDQ5WjAXMRUwEwYDVQQDDAxJUCBhcyBhIG5hbWUwWTATBgcqhkjOPQIB\n"
         "BggqhkjOPQMBBwNCAAQPaKtBzlWdZtx61nC6SO+V0auRBaa93ZZtBDS9U+GAZPjC\n"
         "nB4Fkm0dApfy1KbxG0tYX/jvhJjW4ba2obvl+ZHAo3oweDAdBgNVHQ4EFgQUydT4\n"
         "a6cBZb9rllA83lfqo7dFXj0wHwYDVR0jBBgwFoAUydT4a6cBZb9rllA83lfqo7dF\n"
         "Xj0wDAYDVR0TAQH/BAIwADATBgNVHSUEDDAKBggrBgEFBQcDATATBgNVHREEDDAK\n"
         "gggxMC4xLjIuMzAKBggqhkjOPQQDAgNIADBFAiB3hbrDinotEBqkdAlwq0JsRaza\n"
         "Wb2KsVATb8cZbg8OdAIhALYsf3s8GjjBSz8Tj7shcCo7WXZM4lw9tYGFChz45m2u\n"
         "-----END CERTIFICATE-----\n";

      const char *ipAsCommonName =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBjzCCATWgAwIBAgIUYng8rHLd/4cqGwB03A98bliqmK4wCgYIKoZIzj0EAwIw\n"
         "EzERMA8GA1UEAwwIMTAuMS4yLjMwIBcNMjYwOTI1MTM0MDQ5WhgPMjEyNjA5MDEx\n"
         "MzQwNDlaMBMxETAPBgNVBAMMCDEwLjEuMi4zMFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
         "AQcDQgAE7PtPaAsMD23bAFBjwXp4SbJDKVNihRTql8biFIYnHFVIqTnlkiF8Jz1/\n"
         "zzqVeErkyeztllJpuMvBr7a3ODRw9qNlMGMwHQYDVR0OBBYEFCJBoC5I2yRtpjg7\n"
         "n1JMcvCe4PsXMB8GA1UdIwQYMBaAFCJBoC5I2yRtpjg7n1JMcvCe4PsXMAwGA1Ud\n"
         "EwEB/wQCMAAwEwYDVR0lBAwwCgYIKwYBBQUHAwEwCgYIKoZIzj0EAwIDSAAwRQIg\n"
         "B3gwgsIz8uPLNc7lsCKBlf8aGaUPivSUmAa11bV6O64CIQDKz93+qKMrQ0aTL32z\n"
         "78lrYfXQpa2o0S6DrPBZEtFqxQ==\n"
         "-----END CERTIFICATE-----\n";

      const char *dnsOnly =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBsTCCAVigAwIBAgIUUwCYXW4uX6DKp74mWGFO0emy6TMwCgYIKoZIzj0EAwIw\n"
         "GDEWMBQGA1UEAwwNRE5TIG9ubHkgdGVzdDAgFw0yNjA5MjUxMzQwNDlaGA8yMTI2\n"
         "MDkwMTEzNDA0OVowGDEWMBQGA1UEAwwNRE5TIG9ubHkgdGVzdDBZMBMGByqGSM49\n"
         "AgEGCCqGSM49AwEHA0IABM4wHJj19guQaRfLA3S+GV3aYO36ggp0nxVEmTwfNBCd\n"
         "Xr62s12HDqZOiI4D5j0+qMHVmhTlrm7pnyEhtQkZfqmjfjB8MB0GA1UdDgQWBBQE\n"
         "WZdz1GM+JQKX+EkZiLQUn+T+qzAfBgNVHSMEGDAWgBQEWZdz1GM+JQKX+EkZiLQU\n"
         "n+T+qzAMBgNVHRMBAf8EAjAAMBMGA1UdJQQMMAoGCCsGAQUFBwMBMBcGA1UdEQQQ\n"
         "MA6CDGV4YW1wbGUudGVzdDAKBggqhkjOPQQDAgNHADBEAiAWLtQmBS+0AiK+Mdbt\n"
         "LWnIIvHd8CblHlAPLf70AD2rTQIgTdixLJ25as4j2GL8RHK3SdIdrbHrJZ2WAPjq\n"
         "ADPA2IE=\n"
         "-----END CERTIFICATE-----\n";

      const char *addressAndOtherCommonName =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBoTCCAUagAwIBAgIUalno+U9oOgM1fpvyvLYc9mKk5wgwCgYIKoZIzj0EAwIw\n"
         "EzERMA8GA1UEAwwIMTAuMS4yLjQwIBcNMjYwOTI1MTYwMDA4WhgPMjEyNjA5MDEx\n"
         "NjAwMDhaMBMxETAPBgNVBAMMCDEwLjEuMi40MFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
         "AQcDQgAEzops53FujbERCGEHg72nUzNmwfFp/xuh6lLehCgQTQZo9b6O/5tR131f\n"
         "TS/C/X3MNPhHn47sEE9+hLaBgXCuu6N2MHQwHQYDVR0OBBYEFIkrbj19cn96jvkc\n"
         "4f57+7CLaNRCMB8GA1UdIwQYMBaAFIkrbj19cn96jvkc4f57+7CLaNRCMAwGA1Ud\n"
         "EwEB/wQCMAAwEwYDVR0lBAwwCgYIKwYBBQUHAwEwDwYDVR0RBAgwBocECgECAzAK\n"
         "BggqhkjOPQQDAgNJADBGAiEAzmOH1S95h7DKNM6mOfVvWCmI05/KrA8mOIfTaNnU\n"
         "vbwCIQCGgx7GjCMemQCbB+noZW2oQvTI28qfVdRHu1rw7+e3eg==\n"
         "-----END CERTIFICATE-----\n";

      const char *wildcardOverAddresses =
         "-----BEGIN CERTIFICATE-----\n"
         "MIIBwTCCAWegAwIBAgIUPE6itASXwxEgfu/L4qA0vqSdhtIwCgYIKoZIzj0EAwIw\n"
         "IjEgMB4GA1UEAwwXV2lsZGNhcmQgb3ZlciBhZGRyZXNzZXMwIBcNMjYwOTI1MTYw\n"
         "MDA4WhgPMjEyNjA5MDExNjAwMDhaMCIxIDAeBgNVBAMMF1dpbGRjYXJkIG92ZXIg\n"
         "YWRkcmVzc2VzMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEmXDCM0lm8lQBYY1o\n"
         "dtMULKFqg1JEXoLsDA0jTYajEIthb0H3PZOufQs+K7/11xpcpD9fc4VcbNKMjq6z\n"
         "8yl3zKN5MHcwHQYDVR0OBBYEFJ+RiLz1nYmHuui36bJN2i9kr43BMB8GA1UdIwQY\n"
         "MBaAFJ+RiLz1nYmHuui36bJN2i9kr43BMAwGA1UdEwEB/wQCMAAwEwYDVR0lBAww\n"
         "CgYIKwYBBQUHAwEwEgYDVR0RBAswCYIHKi4xLjIuMzAKBggqhkjOPQQDAgNIADBF\n"
         "AiEApYU1i2BVzoNANjrQhlxDnhRvIWGgLRCH8VEPwJyQKf0CIBmyEeEQHX+rXw+d\n"
         "KCIM+xxKWm+UUNGZ+aBlho4sKk/x\n"
         "-----END CERTIFICATE-----\n";
      // The finding (docs/TestLab.md, "What the first runs found", 7): an endpoint
      // named by its address, whose certificate names that address as an
      // iPAddress entry and nowhere else - what a public CA issues for an
      // address. X509_check_host alone answered no to the first of these.
      Assert::IsTrue(Matches_(ipOnly, "10.1.2.3"));
      Assert::IsFalse(Matches_(ipOnly, "10.1.2.4"));
      Assert::IsFalse(Matches_(ipOnly, "example.test"));
      Assert::IsFalse(Matches_(ipOnly, ""));

      // IPv6, short and long, and in a URL's brackets; compared as addresses.
      Assert::IsTrue(Matches_(ipv6Only, "2001:db8::7"));
      Assert::IsTrue(Matches_(ipv6Only, "2001:0db8:0000:0000:0000:0000:0000:0007"));
      Assert::IsTrue(Matches_(ipv6Only, "[2001:db8::7]"));
      Assert::IsFalse(Matches_(ipv6Only, "2001:db8::8"));
      Assert::IsFalse(Matches_(ipv6Only, "10.1.2.3"));

      // What was accepted before still is, as CryptoAPI accepts it on Windows: an
      // address written as a dNSName, and as the common name of a certificate
      // with no subjectAltName at all.
      Assert::IsTrue(Matches_(ipAsDnsName, "10.1.2.3"));
      Assert::IsTrue(Matches_(ipAsCommonName, "10.1.2.3"));
      Assert::IsFalse(Matches_(ipAsCommonName, "10.1.2.4"));

      // But not the common name of a certificate that names addresses as
      // addresses, which X509_check_host reads whenever there is no dNSName
      // entry; and never a wildcard, which X509_check_host alone took *.1.2.3
      // to be for 10.1.2.3 (both measured on OpenSSL 4.0.2, 25 September 2026).
      Assert::IsTrue(Matches_(addressAndOtherCommonName, "10.1.2.3"));
      Assert::IsFalse(Matches_(addressAndOtherCommonName, "10.1.2.4"));
      Assert::IsFalse(Matches_(wildcardOverAddresses, "10.1.2.3"));

      // And a name is matched as a name, without regard to case, and never by an
      // address.
      Assert::IsTrue(Matches_(dnsOnly, "example.test"));
      Assert::IsTrue(Matches_(dnsOnly, "EXAMPLE.test"));
      Assert::IsFalse(Matches_(dnsOnly, "other.test"));
      Assert::IsFalse(Matches_(dnsOnly, "10.1.2.3"));
   }

}
